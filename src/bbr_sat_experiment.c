/*
 * bbr_sat_experiment.c -- Single-scenario experiment runner for BBR-SAT.
 *
 * Implements bbr_sat_experiment_run(): a continuous picoquic simulation
 * that fires a handover signal at a configurable time and outputs one CSV
 * row per run to stdout. Designed to be invoked by the Python harness via
 * subprocess per experiment condition.
 *
 * CSV output format (one row per run):
 *   baseline,orbit_from,orbit_to,handover_time_s,lead_time_s,loss_pct,seed,
 *   t90_us,goodput_bytes,peak_queue_depth,loss_events,converged
 *
 * Baselines:
 *   0 = B1 (Vanilla BBRv3)
 *   2 = B3 (cwnd-freeze)
 *   3 = B4 (pause/resume)
 *   4 = BBR-SAT (full mechanism)
 *
 * Registration: add to picoquictest.h and picoquic_t.c (see bottom of file).
 */

#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "tls_api.h"
#include "picoquictest_internal.h"
#ifdef _WINDOWS
#include "wincompat.h"
#endif
#include <picotls.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "picoquic_binlog.h"
#include "csv.h"
#include "qlog.h"
#include "autoqlog.h"
#include "picoquic_logger.h"
#include "performance_log.h"
#include "picoquictest.h"
#include "picoquic_bbr.h"
#include "picoquic_cubic.h"

/* =========================================================================
 * Experiment constants -- must match BBR-SAT orbit table defaults
 * ========================================================================= */

/* One-way latency in microseconds */
#define EXP_LEO_LATENCY_US   25000ULL
#define EXP_MEO_LATENCY_US   80000ULL
#define EXP_GEO_LATENCY_US  290000ULL

/* Download BW in Mbps (server→terminal, s_to_c link) */
#define EXP_LEO_DL_MBPS  50ULL
#define EXP_MEO_DL_MBPS  30ULL
#define EXP_GEO_DL_MBPS  10ULL

/* Upload BW in Mbps (terminal→server, c_to_s link) — BBR sender measures this */
#define EXP_LEO_UL_MBPS  10ULL
#define EXP_MEO_UL_MBPS  10ULL
#define EXP_GEO_UL_MBPS   3ULL

/* Queue depth: 1 BDP */
#define EXP_QUEUE_DEPTH_BDP 1.0

/* Data size -- large enough to never complete during the experiment window */
#define EXP_DATA_SIZE  (500 * 1024 * 1024ULL)  /* 500 MB */

/* Baseline IDs */
#define EXP_BASELINE_B1      0   /* Vanilla BBRv3 */
#define EXP_BASELINE_B3      2   /* cwnd-freeze */
#define EXP_BASELINE_B4      3   /* pause/resume */
#define EXP_BASELINE_BBRSAT  4   /* BBR-SAT full */
#define EXP_BASELINE_CUBIC   5   /* Vanilla CUBIC */

/* T90 measurement: time for throughput to recover to 90% of new orbit fair share */
/* Measured as time from handover_time to first moment where
 * delivered_bytes_rate >= 0.9 * target_bw_bps */
#define EXP_T90_WINDOW_US    1000000ULL  /* 1-second measurement window */
#define EXP_T90_MAX_US      55000000ULL  /* give up after 55s post-handover */

/* =========================================================================
 * Orbit parameter table
 * ========================================================================= */
typedef struct {
    uint64_t latency_us;   /* one-way latency */
    uint64_t dl_mbps;      /* download BW: server→terminal (s_to_c) */
    uint64_t ul_mbps;      /* upload BW:   terminal→server (c_to_s) — BBR measures this */
    int      orbit_class;  /* BBR_SAT_ORBIT_* */
} exp_orbit_params_t;

static const exp_orbit_params_t exp_orbits[3] = {
    { EXP_LEO_LATENCY_US, EXP_LEO_DL_MBPS, EXP_LEO_UL_MBPS, BBR_SAT_ORBIT_LEO },
    { EXP_MEO_LATENCY_US, EXP_MEO_DL_MBPS, EXP_MEO_UL_MBPS, BBR_SAT_ORBIT_MEO },
    { EXP_GEO_LATENCY_US, EXP_GEO_DL_MBPS, EXP_GEO_UL_MBPS, BBR_SAT_ORBIT_GEO },
};

/* =========================================================================
 * Handover signal helper
 * ========================================================================= */
static void exp_fire_signal(picoquic_cnx_t* cnx, uint64_t signal, uint64_t current_time)
{
    picoquic_per_ack_state_t ack_state;
    memset(&ack_state, 0, sizeof(ack_state));
    ack_state.nb_bytes_acknowledged = signal;
    cnx->congestion_alg->alg_notify(
        cnx, cnx->path[0],
        picoquic_congestion_notification_satellite_handover,
        &ack_state, current_time);
}

/* =========================================================================
 * T90 measurement state
 * ========================================================================= */
typedef struct {
    uint64_t handover_time_us;      /* when handover fired */
    uint64_t target_bw_bps;         /* 90% threshold = 0.9 * this */
    uint64_t window_start_us;       /* start of current 1-s measurement window */
    uint64_t window_bytes;          /* bytes delivered in current window */
    uint64_t prev_delivered;        /* cnx->data_received at window_start */
    uint64_t t90_us;                /* result: 0 if not yet achieved */
    int      achieved;              /* 1 if T90 has been recorded */
} exp_t90_state_t;

static void exp_t90_init(exp_t90_state_t* t90, uint64_t handover_time_us,
                          uint64_t target_bw_bps)
{
    memset(t90, 0, sizeof(*t90));
    t90->handover_time_us = handover_time_us;
    t90->target_bw_bps    = target_bw_bps;
    t90->t90_us           = UINT64_MAX; /* not achieved */
}

/* Update T90 measurement on each sim round.
 * Returns 1 if T90 has been determined (achieved or timed out). */
static int exp_t90_update(exp_t90_state_t* t90, picoquic_cnx_t* cnx,
                           uint64_t current_time)
{
    if (t90->achieved || current_time < t90->handover_time_us) return 0;

    /* Timeout: give up */
    if (current_time > t90->handover_time_us + EXP_T90_MAX_US) {
        t90->t90_us  = UINT64_MAX; /* did not recover */
        t90->achieved = 1;
        return 1;
    }

    /* Initialise window on first call post-handover */
    if (t90->window_start_us == 0) {
        t90->window_start_us = current_time;
        t90->prev_delivered  = cnx->data_received;
        return 0;
    }

    /* Check if window has elapsed */
    if (current_time - t90->window_start_us >= EXP_T90_WINDOW_US) {
        uint64_t bytes_in_window = cnx->data_received - t90->prev_delivered;
        /* bytes_in_window over 1 second = rate in bytes/s = bps/8 */
        uint64_t rate_bps = bytes_in_window * 8;
        uint64_t threshold_bps = (uint64_t)(0.9 * (double)t90->target_bw_bps);

        if (rate_bps >= threshold_bps) {
            /* T90 achieved -- record midpoint of window */
            t90->t90_us   = t90->window_start_us - t90->handover_time_us +
                             EXP_T90_WINDOW_US / 2;
            t90->achieved = 1;
            return 1;
        }

        /* Slide window */
        t90->window_start_us = current_time;
        t90->prev_delivered  = cnx->data_received;
    }
    return 0;
}

/* =========================================================================
 * Core experiment function
 * ========================================================================= */

/*
 * bbr_sat_experiment_one -- run one experiment condition.
 *
 * Parameters:
 *   baseline        -- EXP_BASELINE_B1/B3/B4/BBRSAT
 *   orbit_from      -- 0=LEO, 1=MEO, 2=GEO (initial orbit)
 *   orbit_to        -- 0=LEO, 1=MEO, 2=GEO (target orbit)
 *   handover_time_s -- simulated time at which handover fires (seconds)
 *   lead_time_s     -- how many seconds before handover the signal fires
 *                      (0 = no advance notice, signal fires at handover_time_s)
 *   loss_pct        -- non-congestion loss rate 0-100 (0=none, use 16 for 1%)
 *                      passed as loss_mask shift value: 0=0%, 4=6.25%, 8=0.4%...
 *                      For 0% use 0, for ~1% use has_loss=1 which sets 0x10000000
 *   seed            -- random seed for this run (used in CID for determinism)
 *   run_id          -- run number within this condition (0-29)
 *   total_time_s    -- total simulation time in seconds
 *
 * Writes one CSV row to stdout on success.
 * Returns 0 on success, -1 on failure.
 */
int bbr_sat_experiment_one(
    int      baseline,
    int      orbit_from,
    int      orbit_to,
    int      handover_time_s,
    int      lead_time_s,
    int      has_loss,
    uint64_t seed,
    int      run_id,
    int      total_time_s)
{
    uint64_t simulated_time = 0;
    int ret = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;

    const exp_orbit_params_t* from = &exp_orbits[orbit_from];
    const exp_orbit_params_t* to   = &exp_orbits[orbit_to];

    uint64_t handover_time_us = (uint64_t)handover_time_s * 1000000ULL;
    uint64_t signal_time_us   = handover_time_us -
                                 (uint64_t)lead_time_s * 1000000ULL;
    uint64_t total_time_us    = (uint64_t)total_time_s * 1000000ULL;

    /* Encode parameters into CID for QLOG tracing */
    picoquic_connection_id_t initial_cid = { {0xBB, 0x5A, 0, 0, 0, 0, 0, 0}, 8 };
    initial_cid.id[2] = (uint8_t)baseline;
    initial_cid.id[3] = (uint8_t)((orbit_from & 0x03) | ((orbit_to & 0x03) << 2));
    initial_cid.id[4] = (uint8_t)(handover_time_s & 0xFF);
    initial_cid.id[5] = (uint8_t)(lead_time_s & 0xFF);
    initial_cid.id[6] = has_loss ? 0x01 : 0x00;
    initial_cid.id[7] = (uint8_t)(run_id & 0xFF);

    picoquic_tp_t client_parameters;
    picoquic_tp_t server_parameters;
    memset(&client_parameters, 0, sizeof(picoquic_tp_t));
    picoquic_init_transport_parameters(&client_parameters);
    client_parameters.enable_time_stamp = 3;
    memset(&server_parameters, 0, sizeof(picoquic_tp_t));
    picoquic_init_transport_parameters(&server_parameters);
    server_parameters.enable_time_stamp = 3;

    ret = tls_api_one_scenario_init_ex(&test_ctx, &simulated_time,
        PICOQUIC_INTERNAL_TEST_VERSION_1,
        &client_parameters, &server_parameters, &initial_cid);

    if (ret == 0 && test_ctx == NULL) ret = -1;
    if (ret != 0) return ret;

    /* Configure initial orbit link — upload bottleneck on c_to_s, download on s_to_c */
    test_ctx->c_to_s_link->microsec_latency = from->latency_us;
    test_ctx->c_to_s_link->picosec_per_byte = (1000000ULL * 8) / from->ul_mbps;
    test_ctx->s_to_c_link->microsec_latency = from->latency_us;
    test_ctx->s_to_c_link->picosec_per_byte = (1000000ULL * 8) / from->dl_mbps;
    test_ctx->stream0_flow_release = 1;
    test_ctx->immediate_exit = 1;

    /* Select CCA based on baseline */
    picoquic_congestion_algorithm_t* cca =
        (baseline == EXP_BASELINE_CUBIC) ?
        picoquic_cubic_algorithm : picoquic_bbr_algorithm;
    picoquic_set_default_congestion_algorithm(test_ctx->qserver, cca);
    picoquic_set_congestion_algorithm(test_ctx->cnx_client, cca);

    /* Phase 1: handshake + queue data stream.
     * Step 1: handshake only (no data yet)
     * Step 2: queue EXP_DATA_SIZE bytes on stream 0
     * Step 3: manual sim loop so we can fire handover mid-run */
    ret = tls_api_one_scenario_body_connect(test_ctx, &simulated_time,
                                             0, 2 * from->latency_us);
    if (ret != 0 || test_ctx->cnx_client == NULL || test_ctx->cnx_server == NULL) {
        ret = -1;
        goto cleanup;
    }

    /* Queue the data stream -- stream0_target tells the sim how many bytes to send */
    test_ctx->stream0_target = (size_t)EXP_DATA_SIZE;
    ret = test_api_init_send_recv_scenario(test_ctx, NULL, 0);
    if (ret != 0) {
        ret = -1;
        goto cleanup;
    }

        /* Seed orbit table -- BBR-SAT only */
    if (baseline == EXP_BASELINE_BBRSAT) {
        bbr_sat_init_orbit_table(test_ctx->cnx_client, from->orbit_class);
        bbr_sat_seed_orbit_bw(test_ctx->cnx_client, from->orbit_class,
                               from->ul_mbps * 1000000ULL);
    }

        /* Metrics tracking */
    uint64_t bytes_at_handover  = 0;
    uint64_t goodput_bytes      = 0;
    uint64_t peak_queue_depth   = 0;
    uint64_t loss_events        = 0;
    int      signal_fired       = 0;
    int      link_switched      = 0;
    /* Steady-state window: T+20s to T+55s post-handover */
    uint64_t ss_sum_bps         = 0;  /* sum of per-second rates */
    double   ss_sum_sq_bps      = 0.0;/* sum of squares for variance (double to avoid overflow) */
    int      ss_samples         = 0;  /* number of 1-second samples */

    exp_t90_state_t t90;
    exp_t90_init(&t90, handover_time_us, to->ul_mbps * 1000000ULL);

    /* Main simulation loop -- drive sim round by round for mid-run control */
    int nb_rounds = 0;
    int was_active = 0;

    while (ret == 0 && simulated_time < total_time_us) {
        nb_rounds++;
        ret = tls_api_one_sim_round(test_ctx, &simulated_time, total_time_us, &was_active);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
            break;
        }

        /* Fire handover SIGNAL (advance notice at signal_time_us) */
        if (!signal_fired && simulated_time >= signal_time_us &&
            test_ctx->cnx_client->cnx_state >= picoquic_state_ready) {

            switch (baseline) {
            case EXP_BASELINE_B1:
                /* B1: vanilla BBRv3 -- no signal, do nothing */
                break;
            case EXP_BASELINE_B3:
                /* B3: cwnd-freeze on signal */
                exp_fire_signal(test_ctx->cnx_client, BBR_SAT_SIG_FREEZE,
                                simulated_time);
                break;
            case EXP_BASELINE_B4:
                /* B4: pause on signal */
                exp_fire_signal(test_ctx->cnx_client, BBR_SAT_SIG_PAUSE,
                                simulated_time);
                break;
            case EXP_BASELINE_BBRSAT:
                /* BBR-SAT Phase 1: PREDICTED -- queue drain only */
                exp_fire_signal(test_ctx->cnx_client,
                                (uint64_t)(0x30 + to->orbit_class),
                                simulated_time);
                break;
            }
            signal_fired = 1;
        }

        /* Switch link parameters at actual handover time */
        if (!link_switched && simulated_time >= handover_time_us) {
            /* Record bytes at handover time for goodput window start */
            bytes_at_handover = (test_ctx->cnx_server != NULL) ?
                test_ctx->cnx_server->data_received : 0;


            /* Switch link to target orbit */
            test_ctx->c_to_s_link->microsec_latency = to->latency_us;
            test_ctx->c_to_s_link->picosec_per_byte = (1000000ULL * 8) / to->ul_mbps;
            test_ctx->s_to_c_link->microsec_latency = to->latency_us;
            test_ctx->s_to_c_link->picosec_per_byte = (1000000ULL * 8) / to->dl_mbps;

            /* BBR-SAT Phase 2: CONFIRMED -- full BDP context switch.
             * Fires at actual link-switch time on the new path.
             * Loads target min_rtt + max_bw, resets stale state. */
            if (baseline == EXP_BASELINE_BBRSAT) {
                exp_fire_signal(test_ctx->cnx_client,
                                (uint64_t)to->orbit_class,
                                simulated_time);
            }
            /* B3: unfreeze at link-switch time */
            if (baseline == EXP_BASELINE_B3) {
                exp_fire_signal(test_ctx->cnx_client, BBR_SAT_SIG_UNFREEZE,
                                simulated_time);
            }
            /* B4: resume at link-switch time */
            if (baseline == EXP_BASELINE_B4) {
                exp_fire_signal(test_ctx->cnx_client, BBR_SAT_SIG_RESUME,
                                simulated_time);
            }

            /* For B3/B4: unfreeze/resume at actual handover time */
            if (baseline == EXP_BASELINE_B3) {
                exp_fire_signal(test_ctx->cnx_client, BBR_SAT_SIG_UNFREEZE,
                                simulated_time);
            } else if (baseline == EXP_BASELINE_B4) {
                exp_fire_signal(test_ctx->cnx_client, BBR_SAT_SIG_RESUME,
                                simulated_time);
            }

            link_switched = 1;
        }

        /* Track peak queue depth (bytes in transit as proxy) */
        if (test_ctx->cnx_client->path[0] != NULL) {
            uint64_t bif = test_ctx->cnx_client->path[0]->bytes_in_transit;
            if (bif > peak_queue_depth) peak_queue_depth = bif;
        }

        /* Track loss events post-handover */
        if (link_switched) {
            loss_events = test_ctx->cnx_client->nb_retransmission_total;
            (void)loss_events; /* used in CSV output below */
        }

        /* Update T90 measurement -- use server as data sink */
        if (link_switched && test_ctx->cnx_server != NULL) {
            exp_t90_update(&t90, test_ctx->cnx_server, simulated_time);
        }
        /* Per-second rate logging + steady-state tracking */
        static uint64_t rate_log_next = 0;
        static uint64_t rate_log_prev_bytes = 0;
        if (link_switched && test_ctx->cnx_server != NULL) {
            if (rate_log_next == 0) {
                rate_log_next = handover_time_us + 1000000ULL;
                rate_log_prev_bytes = test_ctx->cnx_server->data_received;
            } else if (simulated_time >= rate_log_next) {
                uint64_t cur = test_ctx->cnx_server->data_received;
                uint64_t rate_bps = (cur - rate_log_prev_bytes) * 8;
                uint64_t t_post_us = simulated_time - handover_time_us;
                uint64_t t_post_s = t_post_us / 1000000ULL;
                fprintf(stderr, "RATE_LOG: t+%llus rate=%llu bps (%.2f Mbps)\n",
                    (unsigned long long)t_post_s,
                    (unsigned long long)rate_bps,
                    (double)rate_bps / 1e6);
                /* Steady-state window: T+20s to T+55s */
                if (t_post_s >= 20 && t_post_s <= 55) {
                    ss_sum_bps    += rate_bps;
                    ss_sum_sq_bps += (double)rate_bps * (double)rate_bps;
                    ss_samples++;
                }
                rate_log_prev_bytes = cur;
                rate_log_next += 1000000ULL;
            }
        }

        /* Sample goodput at end of 55s post-handover window */
        if (link_switched &&
            simulated_time >= handover_time_us + 55000000ULL
            && goodput_bytes == 0 && test_ctx->cnx_server != NULL) {
            uint64_t post_bytes = test_ctx->cnx_server->data_received;
            goodput_bytes = (post_bytes > bytes_at_handover) ?
                post_bytes - bytes_at_handover : 0;
        }
    }

    ret = 0; /* ignore partial completion */

    /* Compute cumulative goodput over 60s window centered on handover */
cleanup:
    /* goodput_bytes sampled 30s post-handover in sim loop above */
    if (goodput_bytes == 0 && test_ctx != NULL && test_ctx->cnx_server != NULL
        && test_ctx->cnx_server->data_received > bytes_at_handover) {
        goodput_bytes = test_ctx->cnx_server->data_received - bytes_at_handover;
    }

    int converged = (t90.t90_us != UINT64_MAX) ? 1 : 0;
    uint64_t t90_out = converged ? t90.t90_us : 0;

    /* Diagnostics -- written to stderr, not captured by harness */
    fprintf(stderr, "DIAG: t_end=%" PRIu64 "us server_total=%" PRIu64
            " bytes_at_ho=%" PRIu64 " goodput=%" PRIu64
            " link_switched=%d signal_fired=%d\n",
        simulated_time,
        (test_ctx && test_ctx->cnx_server) ?
            test_ctx->cnx_server->data_received : 0,
        bytes_at_handover,
        goodput_bytes,
        link_switched, signal_fired);
    /* BBR state diagnostics (BBR baselines only) */
    if (test_ctx && test_ctx->cnx_client &&
        baseline != EXP_BASELINE_CUBIC) {
        fprintf(stderr, "DIAG: bbr_state=%d max_bw=%" PRIu64
            " min_rtt=%" PRIu64 " filled=%d\n",
            bbr_sat_get_state(test_ctx->cnx_client),
            bbr_sat_get_max_bw(test_ctx->cnx_client),
            bbr_sat_get_min_rtt(test_ctx->cnx_client),
            bbr_sat_get_filled_pipe(test_ctx->cnx_client));
    }
    /* Compute steady-state mean and stddev */
    uint64_t ss_mean_bps = 0;
    uint64_t ss_stddev_bps = 0;
    if (ss_samples > 0) {
        ss_mean_bps = ss_sum_bps / (uint64_t)ss_samples;
        if (ss_samples > 1) {
            double mean_d = (double)ss_mean_bps;
            double var_d  = (ss_sum_sq_bps / (double)ss_samples) -
                            (mean_d * mean_d);
            if (var_d > 0.0) {
                ss_stddev_bps = (uint64_t)__builtin_sqrt(var_d);
            }
        }
    }

    /* Output CSV row */
    printf("%d,%d,%d,%d,%d,%d,%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64 "\n",
        baseline, orbit_from, orbit_to,
        handover_time_s, lead_time_s,
        has_loss ? 1 : 0,
        seed, run_id,
        t90_out, goodput_bytes,
        peak_queue_depth, converged,
        ss_mean_bps, ss_stddev_bps);
    fflush(stdout);

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    return ret;
}

/* =========================================================================
 * Registered test entry points (smoke tests for CI)
 * These run one condition each to verify the experiment function works.
 * The full sweep is driven by the Python harness.
 * ========================================================================= */

/* B1 smoke test: LEO->GEO, handover at T=30s, no lead time, no loss */
int bbr_sat_exp_b1_smoke_test(void)
{
    /* Suppress CSV output in test mode by redirecting -- just check ret */
    return bbr_sat_experiment_one(
        EXP_BASELINE_B1,
        BBR_SAT_ORBIT_LEO, BBR_SAT_ORBIT_GEO,
        30, 0, 0, 42, 0, 90);
}

/* BBR-SAT smoke test: LEO->GEO, handover at T=30s, 5s lead, no loss */
int bbr_sat_exp_bbrsat_smoke_test(void)
{
    return bbr_sat_experiment_one(
        EXP_BASELINE_BBRSAT,
        BBR_SAT_ORBIT_LEO, BBR_SAT_ORBIT_GEO,
        30, 5, 0, 42, 0, 90);
}

/* B3 smoke test: LEO->GEO, handover at T=30s, 5s lead, no loss */
int bbr_sat_exp_b3_smoke_test(void)
{
    return bbr_sat_experiment_one(
        EXP_BASELINE_B3,
        BBR_SAT_ORBIT_LEO, BBR_SAT_ORBIT_GEO,
        30, 5, 0, 42, 0, 90);
}

/* B4 smoke test: LEO->GEO, handover at T=30s, 5s lead, no loss */
int bbr_sat_exp_b4_smoke_test(void)
{
    return bbr_sat_experiment_one(
        EXP_BASELINE_B4,
        BBR_SAT_ORBIT_LEO, BBR_SAT_ORBIT_GEO,
        30, 5, 0, 42, 0, 90);
}

/*
 * Registration instructions:
 *
 * In picoquictest/picoquictest.h, add after bbr_sat_bw_ceiling_test:
 *   int bbr_sat_exp_b1_smoke_test(void);
 *   int bbr_sat_exp_bbrsat_smoke_test(void);
 *   int bbr_sat_exp_b3_smoke_test(void);
 *   int bbr_sat_exp_b4_smoke_test(void);
 *
 * In picoquic_t/picoquic_t.c, add after bbr_sat_bw_ceiling entry:
 *   { "bbr_sat_exp_b1_smoke", bbr_sat_exp_b1_smoke_test },
 *   { "bbr_sat_exp_bbrsat_smoke", bbr_sat_exp_bbrsat_smoke_test },
 *   { "bbr_sat_exp_b3_smoke", bbr_sat_exp_b3_smoke_test },
 *   { "bbr_sat_exp_b4_smoke", bbr_sat_exp_b4_smoke_test },
 *
 * In CMakeLists.txt, add after bbr_sat_test.c:
 *   picoquictest/bbr_sat_experiment.c
 */
