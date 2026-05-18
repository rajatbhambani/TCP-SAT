/*
* Author: Christian Huitema
* Copyright (c) 2019, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>
#include "cc_common.h"
#include "picoquic_utils.h"

/* BBR-SAT orbit class constants -- also defined in picoquic_bbr.h for external use */
#define BBR_SAT_ORBIT_LEO 0
#define BBR_SAT_ORBIT_MEO 1
#define BBR_SAT_ORBIT_GEO 2

#ifdef BBRExperiment
#define BBRExpGate(ctx, test, action) { if (!ctx->exp_flags.test) action; }
#define BBRExpTest(ctx, test) ( (ctx)->exp_flags.test )
#else
#define BBRExpGate(ctx, test, action) {}
#define BBRExpTest(ctx, test) (1)
#endif

#define RTTJitterBuffer On
#define RTTJitterBufferStartup On
#define RTTJitterBufferProbe On

/*
Implementation of the BBR3 algorithm, tuned for Picoquic.
Based on https://datatracker.ietf.org/doc/html/draft-cardwell-iccrg-bbr-congestion-control-02,
which describes BBR2 but incorporates the "bug fixes" that
differentiate BBR3 from BBR2.

Early testing showed that BBR startup phase requires several more RTT
than the Hystart process used in modern versions of Reno or Cubic. BBR
only ramps up the data rate after the first bandwidth measurement is
available, 2*RTT after start, while Reno or Cubic start ramping up
after just 1 RTT. BBR only exits startup if three consecutive RTT
pass without significant BW measurement increase, which not only
adds delay but also creates big queues as data is sent at 2.77 times
the bottleneck rate. This is a tradeoff: longer search for bandwidth in
slow start is less likely to stop too early because of transient
issues, but one high bandwidth and long delay links this translates
to long delays and a big batch of packet losses.

This BBR implementation addresses these issues by switching to
Hystart instead of startup if the RTT is above the Reno target of
250 ms. 
*/

/* Reaction to losses and ECN
* 
* TODO: port the BBRv1 code.
 */

/*
* Handling of suspension
* 
* Suspension is handled "almost" as specified in BBRv3. On an RTO (or PTO)
* timer, the "revovery" and "pto" flags are set, and the CWIN is reduced
* to "bytes in transit + 1 packet". If a second timer occurs, the
* CWIN is progressively reduced to 1 packet.
* 
* The code exits the PTO situation if the packet that triggered the PTO
* is acknowledged -- or if a later packet is acknowledged. The flags are reset,
* the old version of the CWIN is restored, and BBR re-enters "startup" mode.
* TODO: remember the prior bandwidth, and use the "BDP seed" mechanism to
* accelerate the start-up phase.
*/

typedef enum {
    picoquic_bbr_alg_startup = 0,
    picoquic_bbr_alg_drain,
    /* picoquic_bbr_alg_probe_bw, */
    picoquic_bbr_alg_probe_bw_down,
    picoquic_bbr_alg_probe_bw_cruise,
    picoquic_bbr_alg_probe_bw_refill,
    picoquic_bbr_alg_probe_bw_up,
    picoquic_bbr_alg_probe_rtt,
    picoquic_bbr_alg_startup_long_rtt,
    picoquic_bbr_alg_startup_resume
} picoquic_bbr_alg_state_t;

typedef enum {
    picoquic_bbr_acks_probe_starting = 0,
    picoquic_bbr_acks_probe_stopping,
    picoquic_bbr_acks_refilling,
    picoquic_bbr_acks_probe_feedback,
} picoquic_bbr_ack_phase_t;

/* Constants in BBRv3 */
#define BBRPacingMarginPercent 1 /* discount factor of 1% used to scale BBR.bw to produce BBR.pacing_rate */

#define BBRLossThresh 0.2 /* maximum tolerated packet loss (default: 20%) */
#define BBRBeta 0.7 /* Multiplicative decrease on packet loss (default: 0.7) */
#define BBRHeadroom 0.15 /* Realive amount of headroom left for other flows. (default: 0.15). (Erroneously set to 0.85 in draft-bbr-02) */
#define BBRMinPipeCwnd 4 /* Default to 4*SMSS, i.e, 4*PMTU */

#define BBRMaxBwFilterLen 2 /* record bw_max for previous cycle and for this one */
#define BBRExtraAckedFilterLen 10 /* to compute the extra acked parameter */

#define BBRMinRTTFilterLen 10000000 /* Length of min rtt filter -- 10 seconds. */
#define BBRRTTJitterBufferLen 7 /* Number of RTT amples retained to filter out jitter */
#define BBRProbeRTTCwndGain 0.5
#define BBRProbeRTTDuration 200000 /* 200msec, 200000 microsecs */
#define BBRProbeRTTInterval 5000000 /* 5 seconds */

#define BBRStartupPacingGain 2.77 /* constant, 4*ln(2), approx 2.77 */
#define BBRStartupCwndGain 2.0 /* constant */
#define BBRStartupIncreaseThreshold 1.25

#define BBRStartupResumePacingGain 1.25 /* arbitrary */
#define BBRStartupResumeCwndGain 1.25 /* arbitrary */
#define BBRStartupResumeIncreaseThreshold 1.125

#define BBRProbeBwDownPacingGain 0.9
#define BBRProbeBwDownCwndGain 2.0
#define BBRProbeBwCruisePacingGain 1.0
#define BBRProbeBwCruiseCwndGain 2.0
#define BBRProbeBwRefillPacingGain 1.0
#define BBRProbeBwRefillCwndGain 2.0
#define BBRProbeBwUpPacingGain 1.25
#define BBRProbeBwUpCwndGain 2.25

#define BBRAppLimitedRoundsThreshold 3

#define BBRMinRttMarginPercent 5 /* Margin factor of 20% for avoiding firing RTT Probe too often */
#define BBRLongRttThreshold 250000

#define BBRExcessiveEcnCE 0.2

/* Temporary code, do define a set of BBR flags that
* turn on and off individual extensions. We want to use that
* to do "before/after" measurements.
 */
#define BBRExperiment on
#ifdef BBRExperiment
 /* Control flags for BBR improvements */
typedef struct st_bbr_exp {
    unsigned int do_early_exit : 1;
    unsigned int do_rapid_start : 1;
    unsigned int do_handle_suspension : 1;
    unsigned int do_control_lost : 1;
    unsigned int do_exit_probeBW_up_on_delay : 1;
    unsigned int do_enter_probeBW_after_limited : 1;
} bbr_exp;
#endif
typedef struct st_picoquic_bbr_state_t {
    /* Algorithm state: */
    picoquic_bbr_alg_state_t state;
    uint64_t round_start_pn;
    int round_count;
    int rounds_since_probe;
    unsigned int round_start : 1;
    uint64_t next_round_delivered; /* packet delivered value at end of round trip */
    /* Output */
    //uint64_t cwnd; /* new in BBRv3 */
    double pacing_rate;
    uint64_t send_quantum;
    uint64_t prior_cwnd;
    /* Pacing state */
    double pacing_gain;
    uint64_t next_departure_time; /* earliest departure time of next packet, per pacing conditions -- new in BBRv3 */
    /* CWND state */
    double cwnd_gain;
    unsigned int packet_conservation : 1; /* whether BBR is using conservation dynamics */
    /*  Data Rate parameters: */
    uint64_t max_bw; /* windowed maximum recent bandwidth sample -- new in BBRv3 */
    uint64_t bw_hi; /* long term maximum -- new in BBRv3 */
    uint64_t bw_lo; /* short term maximum -- new in BBRv3 */
    uint64_t bw; /* max bw for current cycle, min(max_bw, bw_hi, bw_lo) -- new in BBRv3 */

    /* RTT parameters */
    uint64_t min_rtt; /* minimum RTT measured over last 10sec */
#ifdef RTTJitterBuffer
    uint64_t rtt_jitter_buffer[BBRRTTJitterBufferLen];
    uint64_t rtt_jitter_cycle;
    uint64_t rtt_short_term_min;
    uint64_t rtt_short_term_max;
    uint64_t last_rtt_sample_stamp;
    int nb_rtt_excess;
#endif
    /* Data volume parameters:*/
    uint64_t bdp; /* estimate of path BDP, bw* min_rtt  -- new part of state in BBRv3 */
    uint64_t extra_acked; /* estimate of ack aggregation on path -- new in BBRv3 */
    uint64_t offload_budget; /* data necessary for using TSO / GSO(or LRO, GRO) -- new in BBRv3 */
    uint64_t max_inflight; /* data necessary to fully use link, f(bdp, extra_acked, offload_budget, BBRMinPipeCwnd)  -- new in BBRv3 */
    uint64_t inflight_hi; /* long term maximum inflight -- when packet losses are observed -- new in BBRv3 */
    uint64_t inflight_lo; /* short term maximum, generally lower than inflight_hi -- new in BBRv3 */

    /* State for responding to congestion: */
    uint64_t bw_latest; /* 1 roundtrip max of delivered bw  -- new in BBRv3 */
    uint64_t inflight_latest; /* 1 roundtrip max of delivered volume -- new in BBRv3 */

    /* Estimate max_bw */
    uint64_t MaxBwFilter[BBRMaxBwFilterLen]; /* filter tracking maximum of ack.delivery_rate, for estimate max_bw -- new in BBRv3 */
    unsigned int cycle_count; /* for estimating max_bw filter, rotating it. */

    /* estimate extra acked */
    uint64_t extra_acked_interval_start; /* start of interval for which extra acked is tracked,  -- new in BBRv3 */
    uint64_t extra_acked_delivered; /* data delivered since BBR.extra_acked_interval_start,  -- new in BBRv3 */
    uint64_t ExtraACKedFilter[BBRExtraAckedFilterLen]; /* max filter tracking aggregation, -- new in BBRv3 */

    /* startup parameters (only used in startup state) */
    unsigned int filled_pipe : 1;
    uint64_t full_bw; /* baseline max_bw if filled_pipe is true */
    int full_bw_count; /* nb non-app-limited round trips without large increase of full_bw */

    /* probertt parameters */
    uint64_t min_rtt_stamp; /* when last min_rtt was obtained. -- new name in BBRv3 */
    uint64_t probe_rtt_min_delay; /* rtt sample in last interval */
    uint64_t probe_rtt_min_stamp; /* time when probe_rtt_min_delay was obtained */
    uint64_t probe_rtt_done_stamp;
    uint64_t min_rtt_margin; /* Margin of error for min RTT, to avoid spurious expiry of probe RTT timer. */
    unsigned int probe_rtt_expired; /* indicates whether min rtt is due for a refresh */
    unsigned int probe_rtt_round_done;
    unsigned int idle_restart : 1;
    unsigned int path_is_app_limited : 1;

    /* probe BW parameters */
    unsigned int probe_probe_bw_quickly : 1;
    uint64_t bw_probe_wait;
    uint64_t bw_probe_ceiling; /* If bandwidth grows more than ceiling in probe_bw states, redo startup */
    uint64_t cycle_stamp;
    uint32_t rounds_since_bw_probe;
    uint32_t bw_probe_up_cnt;
    uint32_t bw_probe_up_rounds;
    uint32_t bw_probe_samples;
    uint64_t bw_probe_up_acks;
    picoquic_bbr_ack_phase_t ack_phase;
#ifdef RTTJitterBuffer
    /* Management of RTT checks */
    unsigned int rtt_too_high_in_round : 1;
#endif
    /* Management of packet losses and recovery */
    unsigned int loss_in_round : 1;
    unsigned int loss_round_start : 1;
    uint64_t loss_round_delivered;

    unsigned int is_in_recovery;
    unsigned int is_pto_recovery;
    uint64_t recovery_packet_number;
    uint64_t recovery_delivered;

    /* Management of lost feedback */
    unsigned int is_handling_lost_feedback : 1;
    uint64_t cwin_before_lost_feedback;

    /* Management of App limited and transition */
    int app_limited_round_count;
    int app_limited_this_round;

    /* Management of ECN marks */
    uint64_t ecn_ect1_last_round;
    uint64_t ecn_ce_last_round;
    double ecn_alpha;

    /* Per connection random state.*/
    uint64_t random_context;

    /* manage startup long_rtt */
    picoquic_min_max_rtt_t rtt_filter;
    uint64_t bdp_seed;
    unsigned int probe_bdp_seed;
    
    /* Experimental extensions, may or maynot be a good idea. */
    char const* option_string;
    uint64_t wifi_shadow_rtt; /* Shadow RTT used for wifi connections. */
    double quantum_ratio; /* allow application to use a different default than 0.1% of bandwidth (or 1ms of traffic) */
#ifdef BBRExperiment
    /* Control flags for BBR improvements */
    bbr_exp exp_flags;
#endif

    /* BBR-SAT extension: per-orbit state table for cross-orbit handover */
    /* See bbr_sat_orbit_entry_t definition below the struct. */
    /* These fields are zero-initialised by BBROnInit's memset. */
    /* sat_orbit_table populated by bbr_sat_init_orbit_table(). */
    struct {
        uint64_t min_rtt_us;   /* ephemeris-derived RTT, microseconds -- trusted immediately */
        uint64_t max_bw_bps;   /* BW estimate: ceiling (EPHEMERIS) or trusted (MEASURED) */
        int      bw_confidence; /* 0=BBR_SAT_BW_EPHEMERIS, 1=BBR_SAT_BW_MEASURED */
        int      valid;         /* non-zero if entry populated */
    } sat_orbit_table[3];      /* [0]=LEO [1]=MEO [2]=GEO */
    int sat_current_orbit;     /* index into sat_orbit_table for current beam */
    int sat_enabled;           /* non-zero if BBR-SAT extension active */

    /* B3: cwnd-freeze baseline (StarQUIC-style) */
    int      sat_cwnd_frozen;       /* non-zero if cwnd/pacing frozen */
    uint64_t sat_frozen_cwnd;       /* saved cwnd at freeze time */
    double   sat_frozen_pacing_rate;/* saved pacing rate at freeze time */

    /* B4: pause/resume baseline (Creo-style) */
    int      sat_send_paused;       /* non-zero if sending fully paused */
} picoquic_bbr_state_t;

/* BBR v3 assumes that there is state associated with the acknowledgements.
 * BBR assumes that upon reception of an ACK the code immediately
 * schedule transmission of packets that are deemed lost (code was
 * modifed to do that).
 * 
 * From draft-cheng-iccrg-delivery-rate-estimation:
 * data_acked = C.delivered - P.delivered
 *            = path->delivered - packet->delivered_prior;
 * ack_elapsed = C.delivered_time - P.delivered_time
 *            = current_time - packet->delivered_time_prior
 * ack_rate = data_acked / ack_elapsed
 * 
 * ack_elapsed is NOT equal to rtt_sample, because packet->delivered_time_prior
 * may be lower than packet->send_time.
 * 
 * The ack rate is imprecise, because of ACK compression, etc. The Cheng draft
 * suggests:
 * - Define "P.first_sent_time" as the time of the first send in a flight of data,
 * - and "P.sent_time" as the time the final send in that flight of data
 *   (the send that transmits packet "P").
 * The elapsed time for sending the flight is:
 * send_elapsed = (P.sent_time - P.first_sent_time)
 *               = packet->send_time - packet->delivered_sent_prior
 * The delay to receive packets should never be larger than the send rate,
 * so we can use a filter:
 *   delivery_elapsed = max(ack_elapsed, send_elapsed)
 *   delivery_rate = data_acked / delivery_elapsed
 *
 */
typedef struct st_bbr_per_ack_state_t {
    uint64_t delivered; /* volume delivered between acked packet and current time */
    uint64_t delivery_rate;  /* delivery rate sample when packet was just acked. */
    uint64_t rtt_sample;
    uint64_t newly_acked; /* volume of data acked by current ack */
    uint64_t newly_lost; /* volume of data marked lost on ack received */
    uint64_t tx_in_flight; /* estimate of in flight data at the time the packet was sent. */
    uint64_t lost; /* volume lost between transmission of packet and arrival of ACK */
    /* Tracking of ECN */
    uint64_t ecn_ce;
    double ecn_frac;
    double ecn_alpha;
    /* Part of "RS" struct */
    unsigned int is_app_limited : 1; /* App marked limited at time of ACK? */
    unsigned int is_cwnd_limited : 1;
} bbr_per_ack_state_t;

/* Forward definition of key functions */
static int IsInAProbeBWState(picoquic_bbr_state_t* bbr_state);
static int BBRIsProbingBW(picoquic_bbr_state_t* bbr_state);
static void BBREnterProbeBW(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time);
static void BBREnterDrain(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
#if 0
static void BBRHandleRestartFromIdle(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time);
#endif
static void BBREnterProbeBW(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time);
static void BBRStartProbeBW_DOWN(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time);
static void BBRStartProbeBW_CRUISE(picoquic_bbr_state_t* bbr_state);
static void BBRStartProbeBW_REFILL(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x);
static void BBREnterStartup(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRReEnterStartup(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRCheckStartupHighLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs);
static void BBRUpdateRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRStartRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRSetRsFromAckState(picoquic_path_t* path_x, picoquic_per_ack_state_t* ack_state, bbr_per_ack_state_t* rs);
static int IsInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs);
static void BBRHandleInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time);
static uint64_t BBRTargetInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x);
static void BBRInitRoundCounting(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRCheckProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time);
static void BBRUpdateMaxBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs);
static void BBRInitPacingRate(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static void BBRResetCongestionSignals(picoquic_bbr_state_t* bbr_state);
static void BBRResetLowerBounds(picoquic_bbr_state_t* bbr_state);
static uint64_t BBRInflightWithBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain, uint64_t bw);
static void BBRUpdateMaxInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static uint64_t BBRInflightWithHeadroom(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x);
static uint64_t BBRBDPMultiple(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain);
static void BBRAdaptUpperBounds(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time);
static int InLossRecovery(picoquic_bbr_state_t* bbr_state);
static int BBRHasElapsedInPhase(picoquic_bbr_state_t* bbr_state, uint64_t interval, uint64_t current_time);
#ifdef RTTJitterBuffer
static void BBRUpdateRTTJitterBuffer(picoquic_bbr_state_t* bbr_state, bbr_per_ack_state_t* rs, uint64_t current_time);
static void BBRResetRTTJitterBuffer(picoquic_bbr_state_t* bbr_state, uint64_t rtt_init_value, uint64_t current_time);
static int IsRTTTooHigh(picoquic_bbr_state_t* bbr_state);
void bbr_sat_cwnd_freeze(picoquic_cnx_t* cnx, picoquic_path_t* path_x);
void bbr_sat_cwnd_unfreeze(picoquic_cnx_t* cnx);
void bbr_sat_send_pause(picoquic_cnx_t* cnx);
void bbr_sat_send_resume(picoquic_cnx_t* cnx);
/* BBR-SAT Phase 1: HANDOVER_PREDICTED signal handler.
 * Fires at T-lead_time. Drains the queue sized for the current orbit.
 * Does NOT touch min_rtt, max_bw, MaxBwFilter, or orbit table.
 * The path is still the old orbit during the lead window. */
static void bbr_sat_handover_phase1(
    picoquic_bbr_state_t* bbr_state,
    picoquic_path_t* path_x,
    uint64_t current_time)
{
    if (!bbr_state->sat_enabled) return;

    /* Defensive: ensure ProbeBW logic runs */
    if (!bbr_state->filled_pipe) {
        bbr_state->filled_pipe  = 1;
        bbr_state->full_bw      = bbr_state->max_bw;
        bbr_state->full_bw_count = 0;
    }

    /* Clear recovery state to prevent PTO path during drain */
    bbr_state->is_in_recovery     = 0;
    bbr_state->is_pto_recovery    = 0;
    bbr_state->packet_conservation = 0;

    /* Enter ProbeBW_DOWN to drain the queue.
     * bw_probe_ceiling = 2*max_bw to prevent spurious BBRReEnterStartup. */
    bbr_state->bw_probe_ceiling = bbr_state->max_bw * 2;
    BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
}

static void bbr_sat_handover_confirmed(picoquic_bbr_state_t* bbr_state,
    picoquic_path_t* path_x, int target_orbit, uint64_t current_time);
#endif
/* Init processes for BBRv3 */

/* Windowed max filter.
* Several parts of the BBR algorithm use "filters":
* MaxBwFilter[BBRMaxBwFilterLen]: max delivery rate during the last two cycles.
* In the simple case, the value is updated at the end of the cycle (?)
 */

uint64_t update_windowed_max_filter(uint64_t* filter, uint64_t v, unsigned int cycle, unsigned int filterLen)
{
    if (filter[cycle % filterLen] < v) {
        filter[cycle % filterLen] = v;
    }
    for (unsigned int i = 0; i < filterLen; i++) {
        if (filter[i] > v) {
            v = filter[i];
        }
    }
    return v;
}

void start_windowed_max_filter_period(uint64_t* filter, unsigned int cycle, unsigned int filterLen)
{
    filter[cycle % filterLen] = 0;
}

uint64_t update_windowed_min_filter(uint64_t* filter, uint64_t v, unsigned int cycle, unsigned int filterLen)
{
    filter[cycle % filterLen] = v;
    for (unsigned int i = 0; i < filterLen; i++) {
        if (filter[i] < v) {
            v = filter[i];
        }
    }
    return v;
}


/* Init per connection random state.
* Should be initialized to a constant when running in test, to
* something unique when running in production. We do that by
* mixing:
* - the "current time", which is constant in tests but varies in production,
* - the connection type, 1 for client, 0 for server, so that even in tests
*   server and clients use different seeds,
* - the path unique number, so that different paths will use different seeds,
*   even in tests.
*/
static void BBRInitRandom(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    uint64_t random_context = 0xfedcba9876543210ull;
    random_context ^= current_time;
    if (path_x->cnx->client_mode) {
        random_context += 0x0123456789abcdefull;
    }
    if (path_x->unique_path_id > 0 && path_x->unique_path_id != UINT64_MAX) {
        random_context *= (path_x->unique_path_id + 1);
    }
    bbr_state->random_context = random_context;
}

static void BBRInitFullPipe(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->filled_pipe = 0;
    bbr_state->full_bw = 0;
    bbr_state->full_bw_count = 0;
}

/* Initialization of optional variables defined in text string
* Syntax:
* - Single letter options that control the "BBR Experiment"
*   E: do_early_exit
*   R: do_rapid_start
*   H: do_handle_suspension
*   L: do_control_lost
*   D: do_exit_probeBW_up_on_delay
*   A: do_enter_probeBW_after_limited
* - Complex options, ends with ':'
*   T999999999: wifi_shadow_rtt, microseconds
*   Q99999.999: quantum_ratio, %
* 
* The "BBR Experiment" is an attempt to improve behavior of BBR for
* realtime support on some networks, mostly Wi-Fi. The experiment was a
* success, and the corresponding support is on by default, The individual
* option flags can be used to turn off some parts of the experiment,
* for example when doing before/after measurements.
*/
static void BBRSetOptions(picoquic_bbr_state_t* bbr_state)
{
    const char* x = bbr_state->option_string;
#ifdef BBRExperiment
    bbr_state->exp_flags.do_early_exit = 1;
    bbr_state->exp_flags.do_rapid_start = 1;
    bbr_state->exp_flags.do_handle_suspension = 1;
    bbr_state->exp_flags.do_control_lost = 1;
    bbr_state->exp_flags.do_exit_probeBW_up_on_delay = 1;
    bbr_state->exp_flags.do_enter_probeBW_after_limited = 1;
#endif

    if (x != NULL) {
        char c;
        while ((c = *x) != 0) {
            x++;
            switch (c) {
#ifdef BBRExperiment
            case 'E':
                bbr_state->exp_flags.do_early_exit = 0;
                break;
            case 'R':
                bbr_state->exp_flags.do_rapid_start = 0;
                break;
            case 'H':
                bbr_state->exp_flags.do_handle_suspension = 0;
                break;
            case 'L':
                bbr_state->exp_flags.do_control_lost = 0;
                break;
            case 'D':
                bbr_state->exp_flags.do_exit_probeBW_up_on_delay = 0;
                break;
            case 'A':
                bbr_state->exp_flags.do_enter_probeBW_after_limited = 0;
                break;
#endif
            case 'T': {
                /* Reading digits into an uint64_t  */
                uint64_t u = 0;
                while ((c = *x) != 0) {
                    if (c >= '0' && c <= '9') {
                        u *= 10;
                        u += c - '0';
                        x++;
                    }
                    else {
                        break;
                    }
                }
                bbr_state->wifi_shadow_rtt = u;
                break;
            }
            case 'Q': {
                /* Reading digits ad one dot into a double  */
                while ((c = *x) != 0) {
                    double d = 0;
                    double div = 1.0;
                    int dotted = 0;
                    while ((c = *x) != 0) {
                        if (c >= '0' && c <= '9') {
                            if (!dotted) {
                                d *= 10;
                                d += c - '0';
                            }
                            else {
                                div /= 10.0;
                                d += div * (c - '0');
                            }
                            x++;
                        }
                        else if (c == '.') {
                            if (dotted) {
                                break;
                            }
                            else {
                                dotted = 1;
                                x++;
                            }
                        }
                        else {
                            break;
                        }
                    }
                    bbr_state->quantum_ratio = d;
                    break;
                }
            }
            case ':':
                /* Ignore */
                break;
            default:
                break;
            }
        }
    }
}

/* Initialization of the BBR state */
static void BBROnInit(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time, char const * option_string)
{
    /* TODO:
    init_windowed_max_filter(filter = BBR.MaxBwFilter, value = 0, time = 0)
    */
    memset(bbr_state, 0, sizeof(picoquic_bbr_state_t));
    bbr_state->option_string = option_string;

    BBRInitRandom(bbr_state, path_x, current_time);
    /* If RTT was already sampled, use it, other wise set min RTT to infinity */
    if (path_x->smoothed_rtt == PICOQUIC_INITIAL_RTT
        && path_x->rtt_variant == 0) {
        bbr_state->min_rtt = UINT64_MAX;
    }
    else {
        bbr_state->min_rtt = path_x->smoothed_rtt;
    }
#ifdef RTTJitterBuffer
    BBRResetRTTJitterBuffer(bbr_state, bbr_state->min_rtt, current_time);
#endif
    bbr_state->probe_rtt_min_stamp = current_time;
    bbr_state->probe_rtt_min_delay = bbr_state->min_rtt;
    bbr_state->min_rtt_stamp = current_time;
    bbr_state->extra_acked_interval_start = current_time;
    bbr_state->extra_acked_delivered = 0;
    /* Support for the experimental options */
    BBRSetOptions(bbr_state);
    if (bbr_state->quantum_ratio == 0) {
        bbr_state->quantum_ratio = 0.001;
    }

    BBRResetCongestionSignals(bbr_state);
    BBRResetLowerBounds(bbr_state);
    BBRInitRoundCounting(bbr_state, path_x);
    BBRInitFullPipe(bbr_state);
    BBRInitPacingRate(bbr_state, path_x);
    BBREnterStartup(bbr_state, path_x);
}

static void picoquic_bbr_reset(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    BBROnInit(bbr_state, path_x, current_time, bbr_state->option_string);
}

static void picoquic_bbr_init(picoquic_path_t* path_x, char const* option_string, uint64_t current_time)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)malloc(sizeof(picoquic_bbr_state_t));

    path_x->congestion_alg_state = (void*)bbr_state;
    if (bbr_state != NULL) {
        BBROnInit(bbr_state, path_x, current_time, option_string);
    }
}

/* End of init processes for BBr v3 */

/* Release the state of the congestion control algorithm */
static void picoquic_bbr_delete(picoquic_path_t* path_x)
{
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
    }
}

/* Path model functions */

/* Managing PTO and recovery */
/* Discuss. This is already largely handled by the transport code.
 * How much of this is needed?
 */
static void BBRModulateCwndForRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    if (rs->newly_lost > 0) {
        if (path_x->cwin > rs->newly_lost + path_x->send_mtu) {
            path_x->cwin = path_x->cwin - rs->newly_lost;
        }
        else {
            path_x->cwin = path_x->send_mtu;
        }
    }
    if (bbr_state->packet_conservation && path_x->cwin < (path_x->bytes_in_transit + rs->newly_acked)) {
        path_x->cwin = path_x->bytes_in_transit + rs->newly_acked;
    }
}
  

static void BBRBoundCwndForModel(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t cap = UINT64_MAX;
    if (IsInAProbeBWState(bbr_state) &&
        bbr_state->state != picoquic_bbr_alg_probe_bw_cruise) {
        if (bbr_state->inflight_hi > 0) {
            cap = bbr_state->inflight_hi;
        }
    }
    else if (bbr_state->state == picoquic_bbr_alg_probe_rtt ||
        bbr_state->state == picoquic_bbr_alg_probe_bw_cruise) {
        cap = BBRInflightWithHeadroom(bbr_state, path_x);
    }

    /* apply inflight_lo (possibly infinite): */
    if (cap > bbr_state->inflight_lo) {
        cap = bbr_state->inflight_lo;
    }
    if (cap < BBRMinPipeCwnd * path_x->send_mtu) {
        cap = BBRMinPipeCwnd * path_x->send_mtu;
    }
    if (path_x->cwin > cap) {
        path_x->cwin = cap;
    }
}

static uint64_t BBRProbeRTTCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t probe_rtt_cwnd = BBRBDPMultiple( bbr_state, path_x, BBRProbeRTTCwndGain);
    if (probe_rtt_cwnd < BBRMinPipeCwnd * path_x->send_mtu) {
        probe_rtt_cwnd = BBRMinPipeCwnd * path_x->send_mtu;
    }
    return probe_rtt_cwnd;
}

static void BBRBoundCwndForProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
        uint64_t cap = BBRProbeRTTCwnd(bbr_state, path_x);
        if (path_x->cwin > cap) {
            path_x->cwin = cap;
        }
    }
}

static void BBRSetCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    /* B3: cwnd-freeze baseline -- hold cwnd constant during handover window */
    if (bbr_state->sat_cwnd_frozen) {
        path_x->cwin = bbr_state->sat_frozen_cwnd;
        return;
    }
    /* B4: pause/resume -- cwin=0 stops sending */
    if (bbr_state->sat_send_paused) {
        path_x->cwin = path_x->send_mtu; /* minimum -- stack requires non-zero */
        return;
    }
    BBRUpdateMaxInflight(bbr_state, path_x);
    /* TODO: check whether this should be done in every state */
    BBRModulateCwndForRecovery(bbr_state, path_x, rs);
    if (!bbr_state->packet_conservation) {
        if (bbr_state->filled_pipe) {
            path_x->cwin += rs->newly_acked;
            if (path_x->cwin > bbr_state->max_inflight) {
                path_x->cwin = bbr_state->max_inflight;
            }
        }
        else if (bbr_state->state == picoquic_bbr_alg_startup_resume &&
            bbr_state->bdp_seed > path_x->cwin) {
            path_x->cwin = bbr_state->bdp_seed;
        }
        else if (path_x->cwin < bbr_state->max_inflight || path_x->delivered < PICOQUIC_CWIN_INITIAL) {
            path_x->cwin = path_x->cwin+ rs->newly_acked;
        }
        if (path_x->cwin < BBRMinPipeCwnd * path_x->send_mtu) {
            path_x->cwin = BBRMinPipeCwnd * path_x->send_mtu;
        }
    }
    BBRBoundCwndForProbeRTT(bbr_state, path_x);
    BBRBoundCwndForModel(bbr_state, path_x);
}


static uint64_t BBRSaveCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{

    if ( !InLossRecovery(bbr_state) && bbr_state->state != picoquic_bbr_alg_probe_rtt) {
        return path_x->cwin;
    }
    else {
        if (bbr_state->prior_cwnd > path_x->cwin) {
            return bbr_state->prior_cwnd;
        }
        else {
            return path_x->cwin;
        }
    }
}

static uint64_t BBRRestoreCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->prior_cwnd > path_x->cwin) {
        return bbr_state->prior_cwnd;
    }
    else {
        return path_x->cwin;
    }
}

/* 
* Entering recovery sets the "packet_conservation" bit on.
* It is reset to 0 after one round trip. 
 */
static void BBROnEnterFastRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
    uint64_t additional_cwnd = path_x->send_mtu;
    if (rs->newly_acked > additional_cwnd) {
        additional_cwnd = rs->newly_acked;
    }
    path_x->cwin = path_x->bytes_in_transit + additional_cwnd;
    bbr_state->recovery_packet_number = picoquic_cc_get_sequence_number(path_x->cnx, path_x);
    bbr_state->packet_conservation = 1;
    bbr_state->is_in_recovery = 1;
    bbr_state->is_pto_recovery = 0;
    bbr_state->recovery_delivered = path_x->delivered;
}

/* Handling of the "lost feedback" state.
 */
static void BBREnterLostFeedback(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if ((IsInAProbeBWState(bbr_state) || bbr_state->state == picoquic_bbr_alg_drain) &&
        !bbr_state->is_handling_lost_feedback &&
        path_x->cnx->cnx_state == picoquic_state_ready) {
        /* Remembering the old cwin, so the state can be restored when the
        * condition is lifted. */
        bbr_state->cwin_before_lost_feedback = path_x->cwin;
        /* setting the congestion window to exactly the bytes in transit, thus
         * preventing any further transmission until the condition is lifted */
        path_x->cwin = path_x->bytes_in_transit;
        bbr_state->is_handling_lost_feedback = 1;
    }
}

static void BBRExitLostFeedback(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->is_handling_lost_feedback) {
        path_x->cwin = bbr_state->cwin_before_lost_feedback;
        bbr_state->is_handling_lost_feedback = 0;
    }
}

/* In picoquic, the arrival of an RTO maps to a "timer based" packet loss.
* Question: do we want this on a loss signal, or simply when observing
* loss data in ack notification?
 */
static void BBROnEnterRTO(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t lost_packet_number)
{
    if (!bbr_state->is_in_recovery) {
        bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
        bbr_state->is_in_recovery = 1;
    }
    if (!bbr_state->is_pto_recovery) {
        path_x->cwin = path_x->bytes_in_transit + path_x->send_mtu;
        bbr_state->recovery_packet_number = lost_packet_number;
        bbr_state->is_pto_recovery = 1;
        bbr_state->recovery_delivered = path_x->delivered;
    }
}

/* Exit loss recovery
* Could be either on end of the recovery period,
* or in the case of PTO if the loss is declared spurious.
*/
static void BBROnExitRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    if (bbr_state->is_in_recovery) {
        path_x->bandwidth_estimate_max = 0;
        path_x->cwin = BBRRestoreCwnd(bbr_state, path_x);
        bbr_state->recovery_packet_number = UINT64_MAX;
        bbr_state->packet_conservation = 0;

        if (bbr_state->is_pto_recovery && BBRExpTest(bbr_state, do_handle_suspension)) {
            /* TODO:
             * we should try to enter startup with a high enough BW. However, 
             * simple attempts to restore the BW parameters have proven ineffective.
             */
            BBRReEnterStartup(bbr_state, path_x);
        }
        else if(bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
            /* Perform same processing as after encountering a high loss */
            BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
        }
        bbr_state->recovery_delivered = path_x->delivered;
        bbr_state->is_in_recovery = 0;
        bbr_state->is_pto_recovery = 0;
        /* Reset the RTT time stamp, to avoid going into probe RTT during loss events */
        bbr_state->probe_rtt_min_stamp = current_time;
        bbr_state->min_rtt_stamp = current_time;
    }
}

static void BBROnSpuriousLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t lost_packet_number, uint64_t current_time)
{
    if (bbr_state->recovery_packet_number <= lost_packet_number && bbr_state->is_pto_recovery) {
        BBROnExitRecovery(bbr_state, path_x, current_time);
    }
}

static int InLossRecovery(picoquic_bbr_state_t* bbr_state)
{
    return (bbr_state->is_in_recovery);
}

static void BBRCheckRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    if (InLossRecovery(bbr_state)) {
        /* Exit loss recovery if full roundtrip expired */
        if (picoquic_cc_get_ack_number(path_x->cnx, path_x) >= bbr_state->recovery_packet_number) {
            BBROnExitRecovery(bbr_state, path_x, current_time);
        }
    }
    else {
        /* Enter loss recovery if new losses */
        if (IsInflightTooHigh(bbr_state, path_x, rs)) {
            BBROnEnterFastRecovery(bbr_state, path_x, rs);
        }
    }
}

/* Computing the congestion window */
static uint64_t BBRBDPMultipleWithBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain, uint64_t bw)
{
    if (bbr_state->min_rtt == UINT64_MAX) {
        return PICOQUIC_CWIN_INITIAL*path_x->send_mtu; /* no valid RTT samples yet */
    }
    bbr_state->bdp = PICOQUIC_BYTES_FROM_RATE(bbr_state->min_rtt, bw);
    return (uint64_t)(gain * (double)bbr_state->bdp);
}

static uint64_t BBRBDPMultiple(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain)
{
    return BBRBDPMultipleWithBw(bbr_state, path_x, gain, bbr_state->bw);
}

static void BBRUpdateOffloadBudget(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->offload_budget = 3 * bbr_state->send_quantum;
}

static uint64_t BBRQuantizationBudget(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t inflight)
{
    BBRUpdateOffloadBudget(bbr_state);
    if (inflight < bbr_state->offload_budget) {
        inflight = bbr_state->offload_budget;
    }
    if (inflight < BBRMinPipeCwnd * path_x->send_mtu) {
        inflight = BBRMinPipeCwnd * path_x->send_mtu;
    }
    if (bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
        inflight += 2*path_x->send_mtu;
    }
    return inflight;
}

static uint64_t BBRInflightWithBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain, uint64_t bw)
{
    uint64_t inflight = BBRBDPMultipleWithBw(bbr_state, path_x, gain, bw);
    return BBRQuantizationBudget(bbr_state, path_x, inflight);
}

static uint64_t BBRInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, double gain)
{
    return BBRInflightWithBw(bbr_state, path_x, gain, bbr_state->bw);
}

static void BBRUpdateMaxInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /*  The draft mentions here a call to BBRUpdateAggregationBudget(),
    * but does not define that function. Its purpose is apparently to set
    * `extra_acked`, but that variable is computed in
    * BBRUpdateACKAggregation(), which is called as part of 
    * BBRUpdateModelAndState(). There is probably no need to do an extra
    * call here. */
    uint64_t inflight = BBRBDPMultiple(bbr_state, path_x, bbr_state->cwnd_gain);

    inflight += bbr_state->extra_acked;

    if (bbr_state->min_rtt < bbr_state->wifi_shadow_rtt && bbr_state->min_rtt > 0){
        inflight = (uint64_t)(((double)inflight) * ((double)bbr_state->wifi_shadow_rtt) / ((double)bbr_state->min_rtt));
    }
    bbr_state->max_inflight = BBRQuantizationBudget(bbr_state, path_x, inflight);
}

/* Pacing rate functions */
static void BBRInitPacingRate(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /* nominal_bandwidth = InitialCwnd / (SRTT ? SRTT : 1ms); */
    uint64_t initial_rtt = PICOQUIC_INITIAL_RTT; /* 1ms */
    if (path_x->smoothed_rtt != PICOQUIC_INITIAL_RTT || path_x->rtt_variant != 0) {
        initial_rtt = path_x->smoothed_rtt;
    }
    double nominal_bandwidth = ((double)(1000000ull * PICOQUIC_CWIN_INITIAL)) / (double)initial_rtt;
    bbr_state->pacing_rate = BBRStartupPacingGain * nominal_bandwidth;
}

static void BBRSetPacingRateWithGain(picoquic_bbr_state_t* bbr_state, double pacing_gain)
{
    /* B3/B4: freeze pacing rate during handover window */
    if (bbr_state->sat_cwnd_frozen || bbr_state->sat_send_paused) {
        if (bbr_state->sat_cwnd_frozen)
            bbr_state->pacing_rate = bbr_state->sat_frozen_pacing_rate;
        else
            bbr_state->pacing_rate = 0; /* pause: no pacing */
        return;
    }
    double rate = pacing_gain * ((double)(bbr_state->bw * (100 - BBRPacingMarginPercent))) / (double)100;

    if (bbr_state->state == picoquic_bbr_alg_startup_resume &&
        !bbr_state->filled_pipe &&
        bbr_state->bdp_seed > 0) {
        double bdp_rate = (((double)bbr_state->bdp_seed*1000000.0) / (double)bbr_state->min_rtt);
        if (bdp_rate > rate) {
            rate = bdp_rate;
        }
    }

    if (bbr_state->filled_pipe || rate > bbr_state->pacing_rate) {
        bbr_state->pacing_rate = rate;
    }
}

static void  BBRSetPacingRate(picoquic_bbr_state_t* bbr_state)
{
    BBRSetPacingRateWithGain(bbr_state, bbr_state->pacing_gain);
}

static void BBRSetSendQuantum(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    /* 1.2 Mbps = 150 kBps = 150000Bps  */
    uint64_t floor = 2 * path_x->send_mtu;
    if (bbr_state->pacing_rate < 150000) {
        floor = 1 * path_x->send_mtu;
    }
    /* 1 ms = 1000000us/1000 */
    bbr_state->send_quantum = (uint64_t)(bbr_state->pacing_rate * bbr_state->quantum_ratio); 
    if (bbr_state->send_quantum > 0x10000) {
        bbr_state->send_quantum = 0x10000;
    }
    if (bbr_state->send_quantum < floor) {
        bbr_state->send_quantum = floor;
    }
}


/* Path model functions when not probing for bandwith */
  /* Near start of ACK processing: */
static void BBRUpdateLatestDeliverySignals(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    /* BBR.bw_latest = max(BBR.bw_latest, rs.delivery_rate) */
    bbr_state->loss_round_start = 0;
    if (bbr_state->bw_latest < rs->delivery_rate) {
        bbr_state->bw_latest = rs->delivery_rate;
    }
    /* BBR.inflight_latest = max(BBR.inflight_latest, rs.delivered) */
    if (bbr_state->inflight_latest < rs->delivered) {
        bbr_state->inflight_latest = rs->delivered;
    }
    
    uint64_t prior_delivered = path_x->delivered - rs->delivered;
    if (prior_delivered >= bbr_state->loss_round_delivered) {
        bbr_state->loss_round_delivered = path_x->delivered;
        bbr_state->loss_round_start = 1;
    }
}

  /* Near end of ACK processing: */
static void BBRAdvanceLatestDeliverySignals(picoquic_bbr_state_t* bbr_state, bbr_per_ack_state_t * rs) {
    if (bbr_state->loss_round_start) {
        bbr_state->bw_latest = rs->delivery_rate;
        bbr_state->inflight_latest = rs->delivered;
    }
}

static void BBRResetCongestionSignals(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->loss_in_round = 0;
#ifdef RTTJitterBuffer
    bbr_state->rtt_too_high_in_round = 0;
#endif
    bbr_state->bw_latest = 0;
    bbr_state->inflight_latest = 0;
}

/* Handle the first congestion episode in this cycle */
static void BBRInitLowerBounds(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->bw_lo == UINT64_MAX) {
        bbr_state->bw_lo = bbr_state->max_bw;
    }
    if (bbr_state->inflight_lo == UINT64_MAX) {
        bbr_state->inflight_lo = path_x->cwin;
    }
}

/* Adjust model once per round based on loss */
static void BBRLossLowerBounds(picoquic_bbr_state_t* bbr_state)
{
    /* set: bw_lo = max(bw_latest, bw_lo*BBRBeta) */
    bbr_state->bw_lo = (uint64_t)(BBRBeta * (double)bbr_state->bw_lo);
    if (bbr_state->bw_lo < bbr_state->bw_latest) {
        bbr_state->bw_lo = bbr_state->bw_latest;
    }
    /* Set: inflight_lo = max(inflight_latest, BBRBeta * bbr_state->inflight_lo) */
    bbr_state->inflight_lo = (uint64_t)(BBRBeta * (double)bbr_state->inflight_lo);
    if (bbr_state->inflight_lo < bbr_state->inflight_latest) {
        bbr_state->inflight_lo = bbr_state->inflight_latest;
    }
}

/* Once per round-trip respond to congestion */
static void BBRAdaptLowerBoundsFromCongestion(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (BBRIsProbingBW(bbr_state)) {
        return;
    }
#ifdef RTTJitterBufferAdapt
    if (bbr_state->loss_in_round || bbr_state->rtt_too_high_in_round) {
#else
    if (bbr_state->loss_in_round) {
#endif
        BBRInitLowerBounds(bbr_state, path_x);
        BBRLossLowerBounds(bbr_state);
    }
}

/* Update congestion state on every ACK */
static void  BBRUpdateCongestionSignals(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRUpdateMaxBw(bbr_state, path_x, rs);
    if (rs->newly_lost > 0) {
        bbr_state->loss_in_round = 1;
    }
#ifdef RTTJitterBufferAdapt
    if (IsRTTTooHigh(bbr_state)) {
        bbr_state->rtt_too_high_in_round = 1;
    }
#endif
    if (!bbr_state->loss_round_start) {
        return;  /* wait until end of round trip */
    }
    BBRAdaptLowerBoundsFromCongestion(bbr_state, path_x);
    bbr_state->loss_in_round = 0;
#ifdef RTTJitterBufferAdapt
    bbr_state->rtt_too_high_in_round = 0;
#endif
}

static void BBRResetLowerBounds(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->bw_lo = UINT64_MAX;
    bbr_state->inflight_lo = UINT64_MAX;
}
        
static void  BBRBoundBWForModel(picoquic_bbr_state_t* bbr_state) {
    /* set bw = min(max_bw, bw_lo, bw_hi)  */
    bbr_state->bw = bbr_state->max_bw;
    if (bbr_state->bw > bbr_state->bw_lo) {
        bbr_state->bw = bbr_state->bw_lo;
    }
    /* TODO: remove the test bw_hi != 0 once variables properly initialized. */
    if (bbr_state->bw > bbr_state->bw_hi && bbr_state->bw_hi != 0) {
        bbr_state->bw = bbr_state->bw_hi;
    }
}


/* Path Model functions when probing for bandwidth */
static void BBRUpdateMaxBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRUpdateRound(bbr_state, path_x);

    if (rs->delivery_rate >= bbr_state->MaxBwFilter[bbr_state->cycle_count%BBRMaxBwFilterLen] || !rs->is_app_limited) {
        bbr_state->max_bw = update_windowed_max_filter(
            bbr_state->MaxBwFilter, rs->delivery_rate, bbr_state->cycle_count, BBRMaxBwFilterLen);
    }
}

static void BBRAdvanceMaxBwFilter(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->cycle_count++;
    bbr_state->ack_phase = picoquic_bbr_acks_probe_starting;

    /* Should the current cycle value be set to zero? Or do we simply rely on the
     * natural rythm of updates, keeping the old value if we only see app limited updates?
     */
    start_windowed_max_filter_period(bbr_state->MaxBwFilter, bbr_state->cycle_count, BBRMaxBwFilterLen);
}

static void BBRUpdateACKAggregation(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    /* Find excess ACKed beyond expected amount over this interval */
    uint64_t interval = (current_time - bbr_state->extra_acked_interval_start);
    uint64_t expected_delivered = bbr_state->bw * interval;
    /* Reset interval if ACK rate is below expected rate: */
    if (bbr_state->extra_acked_delivered <= expected_delivered) {
        bbr_state->extra_acked_delivered = 0;
        bbr_state->extra_acked_interval_start = current_time;
        expected_delivered = 0;
    }
    bbr_state->extra_acked_delivered += rs->newly_acked;
    uint64_t extra = bbr_state->extra_acked_delivered - expected_delivered;
    if (extra > path_x->cwin) {
        extra = path_x->cwin;
    }
    bbr_state->extra_acked =
        update_windowed_max_filter(bbr_state->ExtraACKedFilter, extra, bbr_state->round_count, BBRExtraAckedFilterLen);
}

/* Do loss signals suggest inflight is too high?
* If so, react.
* This test can trigger spuriously if there are too few packets in transit.
* For example, if there are two packets in transit and one is lost, the
* test assumes a loss rate of 50%, but this could be a random event that
* happens once every 50 RTT. Decisions made because of that would be wrong.
*/
static int IsInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs)
{
    if (rs->ecn_alpha > BBRExcessiveEcnCE) {
        return 1;
    }
    else {
        uint64_t rs_delivered = path_x->delivered - rs->delivered;
        if (rs_delivered > bbr_state->recovery_delivered &&
            rs->lost > (uint64_t)(((double)rs->tx_in_flight) * BBRLossThresh) &&
            rs->lost > 3 * path_x->send_mtu) {
            return 1;
        }
        else {
            return 0;
        }
    }
}

static void BBRHandleInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    /* The computation below compares the number of bytes in flight when the 
     * acked packet was sent to the current target */
    bbr_state->bw_probe_samples = 0;  /* only react once per bw probe */
    if (!rs->is_app_limited) {
        uint64_t beta_target = (uint64_t)(((double)BBRTargetInflight(bbr_state, path_x)) * BBRBeta);
        bbr_state->inflight_hi = (rs->tx_in_flight > beta_target) ? rs->tx_in_flight : beta_target;
    }
    if(bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
        BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
    }
}

static int CheckInflightTooHigh(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    if (IsInflightTooHigh(bbr_state, path_x, rs))
    {
        if (bbr_state->bw_probe_samples)
        {
            BBRHandleInflightTooHigh(bbr_state, path_x, rs, current_time);
        }
        return 1;  /* inflight too high */
    }
    else {
        return 0;
    }
}

/* BBR Round counting functions */
static void BBRInitRoundCounting(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->next_round_delivered = 0;
    bbr_state->round_start = 0;
    bbr_state->round_count = 0;
    bbr_state->round_start_pn = picoquic_cc_get_sequence_number(path_x->cnx, path_x);
}

static void BBRStartRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->round_start_pn = picoquic_cc_get_sequence_number(path_x->cnx, path_x);

    bbr_state->next_round_delivered = path_x->delivered;
}

static void BBRUpdateRound(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (picoquic_cc_get_ack_number(path_x->cnx, path_x) >= bbr_state->round_start_pn) {
        BBRStartRound(bbr_state, path_x);
        bbr_state->round_count++;
        bbr_state->rounds_since_probe++;
        bbr_state->round_start = 1;
        start_windowed_max_filter_period(bbr_state->ExtraACKedFilter, bbr_state->round_count, BBRExtraAckedFilterLen);
    }
    else {
        bbr_state->round_start = 0;
    }
}


/* End of BBR round counting functions */

/* Restart from idle process
* TODO: add a congestion callback "restart from idle" if sending a packet
* after a long silence. The tests should be done in the transport loop. This
* will need to be handled in their own way by all agorithms, and thus cannot
* be implemented in this PR.
* The required call back is mentioned in section 4.1. of RFC 5681,
* Restarting Idle Connections. This is defined for TCP, but should
* apply equally to a QUIC path. the text says "Therefore, a TCP SHOULD
* set cwnd to no more than RW before beginning transmission if the TCP
* has not sent data in an interval exceeding the retransmission timeout."
* For QUIC, this perhaps should be qualified as "as not sent ack-eliciting data".
* The idle test checks "no bytes in transit"; this imply the callback
* should happen before updating "bytes in transit" for the new packet.
 */
#if 0
static void BBRHandleRestartFromIdle(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    if (path_x->bytes_in_transit == 0 && bbr_state->path_is_app_limited)
    {
        bbr_state->idle_restart = 1;
        bbr_state->extra_acked_interval_start = current_time;
        if (IsInAProbeBWState(bbr_state)) {
            BBRSetPacingRateWithGain(bbr_state, 1);
        }
        else if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
            BBRCheckProbeRTTDone(bbr_state, current_time);
        }
    }
}


/* TODO: check definition of app limited. Why is it expressed here?
* The name should really be, check whether transmission has started.
* It is used to differentiate "started and then idle" from "has not
* sent anything yet."
 */
static void MarkConnectionAppLimited(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->path_is_app_limited =
        (C.delivered + packets_in_flight) ?  0 : 1;
}


/* Called when the app has sent something. */
void BBROnTransmit(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    BBRHandleRestartFromIdle(bbr_state, path_x, current_time);
}
#endif
/* End of idle functions */

/* ProbeRTT processes for BBv3 */

/* Adapt RTT min margin based on packet transmission time */
static void BBRAdaptMinRttMargin(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t margin = ((bbr_state->min_rtt * BBRMinRttMarginPercent) * 100 / 1000000);
    if (bbr_state->max_bw > 0) {
        margin += 2 * path_x->send_mtu * 1000000 / bbr_state->max_bw;
    }
    bbr_state->min_rtt_margin = margin;
}

#ifdef RTTJitterBuffer
static void BBRUpdateRTTJitterBuffer(picoquic_bbr_state_t* bbr_state, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    if (current_time > bbr_state->last_rtt_sample_stamp + 1000) {
        bbr_state->rtt_jitter_buffer[bbr_state->rtt_jitter_cycle % BBRRTTJitterBufferLen] = rs->rtt_sample;
        bbr_state->rtt_jitter_cycle++;
        bbr_state->last_rtt_sample_stamp = current_time;
        bbr_state->rtt_short_term_min = UINT64_MAX;
        bbr_state->rtt_short_term_max = 0;
        for (unsigned int i = 0; i < BBRRTTJitterBufferLen; i++) {
            if (i >= bbr_state->rtt_jitter_cycle) {
                break;
            }
            if (bbr_state->rtt_jitter_buffer[i] > bbr_state->rtt_short_term_max) {
                bbr_state->rtt_short_term_max = bbr_state->rtt_jitter_buffer[i];
            }
            if (bbr_state->rtt_jitter_buffer[i] < bbr_state->rtt_short_term_min) {
                bbr_state->rtt_short_term_min = bbr_state->rtt_jitter_buffer[i];
            }
        }
    }
}

static void BBRResetRTTJitterBuffer(picoquic_bbr_state_t* bbr_state,  uint64_t rtt_init_value, uint64_t current_time)
{
    bbr_state->rtt_jitter_cycle = 0;
    bbr_state->last_rtt_sample_stamp = current_time;
    bbr_state->rtt_short_term_min = rtt_init_value;
    bbr_state->rtt_short_term_max = rtt_init_value;
    bbr_state->probe_rtt_min_delay = rtt_init_value;
    bbr_state->nb_rtt_excess = 0;
}

static void BBRUpdateMinRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRAdaptMinRttMargin(bbr_state, path_x);
    /* maintain filter of last BBRRTTJitterBufferLen samples, to handle jitter */
    BBRUpdateRTTJitterBuffer(bbr_state, rs, current_time);
    /* Compute the BBR expired limit */
    if (bbr_state->min_rtt < UINT64_MAX) {
        if (bbr_state->min_rtt <= BBRLongRttThreshold) {
            bbr_state->probe_rtt_expired =
                current_time > bbr_state->probe_rtt_min_stamp + BBRProbeRTTInterval;
        }
        else {
            bbr_state->probe_rtt_expired =
                current_time > bbr_state->probe_rtt_min_stamp + bbr_state->min_rtt * 100;
        }
    }
    /* Update min rtt */
    if (bbr_state->rtt_short_term_max < bbr_state->probe_rtt_min_delay ||
            bbr_state->probe_rtt_expired ||
            bbr_state->rtt_jitter_cycle < BBRRTTJitterBufferLen) {
        bbr_state->probe_rtt_min_delay = bbr_state->rtt_short_term_max;
        bbr_state->probe_rtt_min_stamp = current_time;
    }
    else {
        /* Deviation from BBRv3: test whether the new measurment does not differ from min_rtt
         * by more than a "margin of error, and in that case delay the need to reevaluate min_rtt */
        if (bbr_state->rtt_short_term_min < (bbr_state->min_rtt + bbr_state->min_rtt_margin)) {
            bbr_state->probe_rtt_min_stamp = current_time;
            bbr_state->min_rtt_stamp = current_time;
        }
    }
    int min_rtt_expired =
        current_time > bbr_state->min_rtt_stamp + BBRMinRTTFilterLen;
    if (bbr_state->probe_rtt_min_delay < bbr_state->min_rtt ||
        min_rtt_expired ||
        bbr_state->rtt_jitter_cycle < BBRRTTJitterBufferLen) {
        bbr_state->min_rtt = bbr_state->probe_rtt_min_delay;
        bbr_state->min_rtt_stamp = bbr_state->probe_rtt_min_stamp;
    }

    if (bbr_state->rtt_short_term_min > bbr_state->min_rtt && bbr_state->min_rtt > PICOQUIC_MINRTT_THRESHOLD)
    {
        uint64_t delta_max = PICOQUIC_MINRTT_MARGIN + bbr_state->min_rtt / 4;
        if (bbr_state->rtt_short_term_min > bbr_state->min_rtt + delta_max) {
            bbr_state->nb_rtt_excess++;
        }
    }
    else
    {
        bbr_state->nb_rtt_excess = 0;
    }

}

static int IsRTTTooHigh(picoquic_bbr_state_t* bbr_state)
{
    return (bbr_state->nb_rtt_excess > BBRRTTJitterBufferLen);
}
#else
static void BBRUpdateMinRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRAdaptMinRttMargin(bbr_state, path_x);
    /* Compute the BBR expired limit */
    if (bbr_state->min_rtt < UINT64_MAX) {
        if (bbr_state->min_rtt <= BBRLongRttThreshold) {
            bbr_state->probe_rtt_expired =
                current_time > bbr_state->probe_rtt_min_stamp + BBRProbeRTTInterval;
        }
        else {
            bbr_state->probe_rtt_expired =
                current_time > bbr_state->probe_rtt_min_stamp + bbr_state->min_rtt * 100;
        }
    }

    if (rs->rtt_sample >= 0 && 
       ( rs->rtt_sample < bbr_state->probe_rtt_min_delay ||
            bbr_state->probe_rtt_expired)) {
        /* Update min rtt */
        bbr_state->probe_rtt_min_delay = rs->rtt_sample;
        bbr_state->probe_rtt_min_stamp = current_time;
    }
    else {
        /* Deviation from BBRv3: test whether the new measurment does not differ from min_rtt
        * by more than a "margin of error, and in that case delay the need to reevaluate min_rtt */
        if (rs->rtt_sample < (bbr_state->min_rtt + bbr_state->min_rtt_margin)) {
            bbr_state->probe_rtt_min_stamp = current_time;
            bbr_state->min_rtt_stamp = current_time;
        }
    }
    int min_rtt_expired =
        current_time > bbr_state->min_rtt_stamp + BBRMinRTTFilterLen;
    if (bbr_state->probe_rtt_min_delay < bbr_state->min_rtt ||
        min_rtt_expired) {
        bbr_state->min_rtt = bbr_state->probe_rtt_min_delay;
        bbr_state->min_rtt_stamp = bbr_state->probe_rtt_min_stamp;
    }
}
#endif

static void BBRExitProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    BBRResetLowerBounds(bbr_state);
    path_x->rtt_min = bbr_state->min_rtt;
    if (bbr_state->filled_pipe) {
        BBREnterProbeBW(bbr_state, path_x, current_time);
        BBRStartProbeBW_CRUISE(bbr_state);
    }
    else {
        BBREnterStartup(bbr_state, path_x);
    }
}

static void BBRCheckProbeRTTDone(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    if (bbr_state->probe_rtt_done_stamp != 0 &&
        current_time > bbr_state->probe_rtt_done_stamp)
    {
        /* schedule next ProbeRTT: */
        bbr_state->probe_rtt_min_stamp = current_time;
        path_x->cwin = BBRRestoreCwnd(bbr_state, path_x);
        BBRExitProbeRTT(bbr_state, path_x, current_time);
    }
}

static void BBRHandleProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    /* Ignore low rate samples during ProbeRTT.*/
    /* We do not implement:
    * MarkConnectionAppLimited();
    * because the app_limited status is maintained as part of app logic.
    */
    /* 
    * testing the bytes in flight when the last ACK was sent,
    * as they reflect the size of the queue encountered when
    * measuring the RTT.
    */
    if (bbr_state->probe_rtt_done_stamp == 0 &&
        rs->tx_in_flight <= BBRProbeRTTCwnd(bbr_state, path_x)) {
        /* Wait for at least ProbeRTTDuration to elapse: */
        bbr_state->probe_rtt_done_stamp =
            current_time + BBRProbeRTTDuration;
        /* Wait for at least one round to elapse: */
        bbr_state->probe_rtt_round_done = 0;
        BBRStartRound(bbr_state, path_x);
    }
    else if (bbr_state->probe_rtt_done_stamp != 0) {
        if (bbr_state->round_start) {
            bbr_state->probe_rtt_round_done = 1;
        }
        if (bbr_state->probe_rtt_round_done) {
            BBRCheckProbeRTTDone(bbr_state, path_x, current_time);
        }
    }
}

static void BBREnterProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    bbr_state->state = picoquic_bbr_alg_probe_rtt;
    bbr_state->pacing_gain = 1.0;
    bbr_state->cwnd_gain = BBRProbeRTTCwndGain;  /* 0.5 */
    path_x->is_cca_probing_up = 0;
}

static void BBRCheckProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    if (bbr_state->state != picoquic_bbr_alg_probe_rtt &&
        bbr_state->probe_rtt_expired &&
        !bbr_state->idle_restart) {
        BBREnterProbeRTT(bbr_state, path_x);
        bbr_state->min_rtt = rs->rtt_sample;
        bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
        bbr_state->probe_rtt_done_stamp = 0;
        bbr_state->ack_phase = picoquic_bbr_acks_probe_stopping;
        BBRStartRound(bbr_state, path_x);
    }
    if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
        BBRHandleProbeRTT(bbr_state, path_x, rs, current_time);
    }
    if (rs->delivered > 0) {
        bbr_state->idle_restart = 0;
    }
}

/* ProbeBW specific processes for BBRv3
* There are actually four states, DOWN, CRUISE, REFILL, and UP.
* TODO: Transition strategy between states is highly dependent on hypotheses,
* such as a BDP of about 63 packets. Investigate what to do if the
* BDP is much higher.
*/

static int IsInAProbeBWState(picoquic_bbr_state_t* bbr_state)
{
    picoquic_bbr_alg_state_t state = bbr_state->state;

    return (state == picoquic_bbr_alg_probe_bw_down ||
        state == picoquic_bbr_alg_probe_bw_cruise ||
        state == picoquic_bbr_alg_probe_bw_refill ||
        state == picoquic_bbr_alg_probe_bw_up);
}

static int BBRIsProbingBW(picoquic_bbr_state_t* bbr_state)
{
    picoquic_bbr_alg_state_t state = bbr_state->state;

    return (state == picoquic_bbr_alg_probe_bw_down ||
        state == picoquic_bbr_alg_probe_bw_cruise ||
        state == picoquic_bbr_alg_drain ||
        state == picoquic_bbr_alg_probe_rtt) ? 0 : 1;
}

/* 
* Return a volume of data that tries to leave free
* headroom in the bottleneck buffer or link for
* other flows, for fairness convergence and lower
* RTTs and loss */
static uint64_t BBRInflightWithHeadroom(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (bbr_state->inflight_hi == UINT64_MAX) {
        return UINT64_MAX;
    }

    /* This diverges from draft-bbr-02, but is correct per feedback from BBR authors. */
    uint64_t inflight_with_headroom = (uint64_t)((1.0-BBRHeadroom) * ((double)bbr_state->inflight_hi));
    if (inflight_with_headroom < (BBRMinPipeCwnd*path_x->send_mtu)) {
        inflight_with_headroom = BBRMinPipeCwnd*path_x->send_mtu;
    }
    return inflight_with_headroom;
}

/* Raise inflight_hi slope if appropriate. */
static void BBRRaiseInflightHiSlope(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    uint64_t growth_this_round = path_x->send_mtu << bbr_state->bw_probe_up_rounds;
    bbr_state->bw_probe_up_rounds = (bbr_state->bw_probe_up_rounds + 1 < 30) ? bbr_state->bw_probe_up_rounds + 1 : 30;
    uint32_t up_cnt = (uint32_t)(path_x->cwin / growth_this_round);
    bbr_state->bw_probe_up_cnt = (up_cnt > 1) ? up_cnt : 1;
}

/* Increase inflight_hi if appropriate. */
static void BBRProbeInflightHiUpward(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs)
{
    if (!rs->is_cwnd_limited || path_x->cwin < bbr_state->inflight_hi)
    {
        return;  /* not fully using inflight_hi, so don't grow it */
    }
    bbr_state->bw_probe_up_acks += rs->newly_acked;
    if (bbr_state->bw_probe_up_acks >= bbr_state->bw_probe_up_cnt*path_x->send_mtu) {
        uint64_t delta = (bbr_state->bw_probe_up_acks / bbr_state->bw_probe_up_cnt);
        bbr_state->bw_probe_up_acks -= delta * bbr_state->bw_probe_up_cnt;
        bbr_state->inflight_hi += delta;
    }

    if (bbr_state->round_start){
        BBRRaiseInflightHiSlope(bbr_state, path_x);
    }
}

/* Track ACK state and update BBR.max_bw window and
* BBR.inflight_hi and BBR.bw_hi. */
static void BBRAdaptUpperBounds(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    /*
    picoquic_bbr_acks_probe_starting = 0,
    picoquic_bbr_acks_probe_stopping,
    picoquic_bbr_acks_refilling,
    */
    if (bbr_state->ack_phase == picoquic_bbr_acks_probe_starting && bbr_state->round_start) {
        /* starting to get bw probing samples */
        bbr_state->ack_phase = picoquic_bbr_acks_probe_feedback;
    }
    if (bbr_state->ack_phase == picoquic_bbr_acks_probe_stopping && bbr_state->round_start) {
        /* end of samples from bw probing phase */
        if (IsInAProbeBWState(bbr_state) && !rs->is_app_limited) {
            BBRAdvanceMaxBwFilter(bbr_state);
        }
    }
    if (!CheckInflightTooHigh(bbr_state, path_x, rs, current_time)) {
        /* Loss rate is safe. Adjust upper bounds upward. */
        if (bbr_state->inflight_hi == UINT64_MAX || bbr_state->bw_hi == UINT64_MAX) {
            return; /* no upper bounds to raise */
        }
        if (rs->tx_in_flight > bbr_state->inflight_hi) {
            /* the bytes in flight at the time the packet was sent did not create a queue. */
            bbr_state->inflight_hi = rs->tx_in_flight;
        }
        if (rs->delivery_rate > bbr_state->bw_hi) {
            bbr_state->bw_hi = rs->delivery_rate;
        }
        if (bbr_state->state == picoquic_bbr_alg_probe_bw_up) {
            BBRProbeInflightHiUpward(bbr_state, path_x, rs);
        }
    }
}

/* Time to transition from DOWN to CRUISE? */
static int BBRCheckTimeToCruise(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (path_x->bytes_in_transit > BBRInflightWithHeadroom(bbr_state, path_x)) {
        return 0; /* not enough headroom */
    }
    if (path_x->bytes_in_transit <= BBRInflightWithBw(bbr_state, path_x, 1.0, bbr_state->max_bw)) {
        return 1;  /* inflight <= estimated BDP */
    }
    return 0;
}


/* Randomized decision about how long to wait until
* probing for bandwidth, using round count and wall clock.
* TODO: find an init strategy for the random numbers, so we
*       can have repetitive tests.
*/
static uint64_t BBRRandomIntBetween(picoquic_bbr_state_t* bbr_state, uint64_t low, uint64_t high)
{
    return (low + picoquic_test_uniform_random(&bbr_state->random_context, (high - low) + 1));
}
#if 0
/* Apprently this code is not needed. */
static double BBRRandomFloatBetween(picoquic_bbr_state_t* bbr_state, double low, double high)
{
    uint32_t random_32_bits = (uint32_t)picoquic_test_random(&bbr_state->random_context);
    double random_float = ((double)random_32_bits) / ((double)UINT32_MAX);
    return (low + random_float * (high - low));
}
#endif

static void BBRPickProbeWait(picoquic_bbr_state_t* bbr_state) 
{
    /* Decide random round-trip bound for wait: */
    bbr_state->rounds_since_bw_probe =
        (uint32_t)BBRRandomIntBetween(bbr_state, 0, 1); /* 0 or 1 */
    
    /* Decide the random wall clock bound for wait: */
    if (bbr_state->min_rtt < BBRLongRttThreshold) {
        bbr_state->bw_probe_wait =
            2000000 + BBRRandomIntBetween(bbr_state, 0, 1000000); /* 0..1 sec, in usec */
    }
    else {
        bbr_state->bw_probe_wait =
            8*bbr_state->min_rtt + BBRRandomIntBetween(bbr_state, 0, 4*bbr_state->min_rtt); /* 0..1 sec, in usec */
    }
}

static void BBRPickProbeWaitEarly(picoquic_bbr_state_t* bbr_state)
{
    /* Decide random round-trip bound for wait: */
    bbr_state->rounds_since_bw_probe =
        (uint32_t)BBRRandomIntBetween(bbr_state, 0, 1); /* 0 or 1 */

    /* Decide the random wall clock bound for wait: */
    if (bbr_state->min_rtt < BBRLongRttThreshold) {
        bbr_state->bw_probe_wait =
            bbr_state->min_rtt + BBRRandomIntBetween(bbr_state, 0, BBRLongRttThreshold);
    }
    else {
        bbr_state->bw_probe_wait =
            bbr_state->min_rtt + BBRRandomIntBetween(bbr_state, 0, bbr_state->min_rtt);
    }
}
/* How much data do we want in flight?
* Our estimated BDP, unless congestion cut cwnd. */
static uint64_t BBRTargetInflight(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    return (bbr_state->bdp < path_x->cwin) ? bbr_state->bdp : path_x->cwin;
}

#ifdef RTTJitterBufferProbe
static int BBRCheckPathSaturated(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    if (IsInAProbeBWState(bbr_state) &&
        rs->rtt_sample > 2*bbr_state->min_rtt &&
        bbr_state->rounds_since_bw_probe >= 1 &&
        bbr_state->pacing_rate > 3 * rs->delivery_rate &&
        bbr_state->wifi_shadow_rtt == 0) {
        bbr_state->prior_cwnd = rs->delivered;
        bbr_state->probe_rtt_done_stamp = 0;
        bbr_state->ack_phase = picoquic_bbr_acks_probe_stopping;
        bbr_state->MaxBwFilter[0] = rs->delivery_rate;
        bbr_state->MaxBwFilter[1] = rs->delivery_rate;
        bbr_state->max_bw = rs->delivery_rate;
        bbr_state->full_bw = rs->delivery_rate;
        BBREnterDrain(bbr_state, path_x);
        BBRStartRound(bbr_state, path_x);
        return 1;
    }
    else {
        return 0;
    }
}
#endif


/* Additional code for managing transition out of "app limited"
* If the application remained in "app limited" state for a long time, the
* interaction between application adaptation and bandwidth measurement may
* cause the measured bottleneck rate to drift down. If the application
* starts pushing more data, we will see a transition "out of app limited".
* In this case, we should accelerate the transition to "ProbeBW UP", and
* let the rate quickly adapt to the new requirements of the application
* and the current state of the network.
*
* The potential downside is that an application alternating between
* high activity and silence might probe for bandwidth more quickly
* than an application that steadily sends data. This may or may not be an
* issue if "steady" and "bumpy" share the same bottleneck -- the bumpy
* application will probe more often, but the steady application will
* defend its sending rate more effectively.
*
* We defined an "app limited state" as "being app limited for more
* than BBRAppLimitedRoundsThreshold" rounds. The code maintains the
* "limited rounds" counter, incremented when a round concludes
* in app limited state, and reset when the congestion limit is
* reached. If reset happens in a Probe BW Cruise state, we force
* and immediate transition to Refill.
*/
static int BBRCheckAppLimitedEnded(picoquic_bbr_state_t* bbr_state, bbr_per_ack_state_t* rs)
{
    int app_limited_ended = 0;
    if (bbr_state->round_start) {
        if (bbr_state->app_limited_this_round) {
            bbr_state->app_limited_round_count++;
        }
        else
        {
            app_limited_ended =
                (bbr_state->app_limited_round_count > BBRAppLimitedRoundsThreshold);
            bbr_state->app_limited_round_count = 0;
        }
        bbr_state->app_limited_this_round = 0;
    }
    else {
        bbr_state->app_limited_this_round |= rs->is_app_limited;
    }
    return app_limited_ended;
}

static int BBRIsRenoCoexistenceProbeTime(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    uint64_t reno_rounds = (BBRTargetInflight(bbr_state, path_x)/path_x->send_mtu);
    uint64_t rounds = (reno_rounds < 63) ? reno_rounds : 63;
    return (bbr_state->rounds_since_bw_probe >= rounds);
}

/* Is it time to transition from DOWN or CRUISE to REFILL? */
static int BBRHasElapsedInPhase(picoquic_bbr_state_t* bbr_state, uint64_t interval, uint64_t current_time) {
    return current_time > bbr_state->cycle_stamp + interval;
}

static int BBRCheckTimeToProbeBW(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    if (BBRHasElapsedInPhase(bbr_state, bbr_state->bw_probe_wait, current_time) ||
        BBRIsRenoCoexistenceProbeTime(bbr_state, path_x) ||
        (BBRExpTest(bbr_state, do_enter_probeBW_after_limited) && BBRCheckAppLimitedEnded(bbr_state, rs))) {
        BBRStartProbeBW_REFILL(bbr_state, path_x);
        return 1;
    }
    else {
        return 0;
    }
}

static void BBRStartProbeBW_DOWN(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    bbr_state->pacing_gain = BBRProbeBwDownPacingGain;  /* pace a bit slowly */
    bbr_state->cwnd_gain = BBRProbeBwDownCwndGain;   /* maintain cwnd */
    BBRResetCongestionSignals(bbr_state);
    bbr_state->bw_probe_up_cnt = UINT32_MAX; /* not growing inflight_hi */
    if (bbr_state->probe_probe_bw_quickly && BBRExpTest(bbr_state, do_rapid_start)) {
        BBRPickProbeWaitEarly(bbr_state);
    }
    else {
        BBRPickProbeWait(bbr_state);
    }
    bbr_state->cycle_stamp = current_time;  /* start wall clock */
    bbr_state->ack_phase = picoquic_bbr_acks_probe_stopping;
    BBRStartRound(bbr_state, path_x);
    bbr_state->state = picoquic_bbr_alg_probe_bw_down;
    bbr_state->nb_rtt_excess = 0;
    bbr_state->app_limited_round_count = 0;
    bbr_state->app_limited_this_round = 0;

    path_x->is_cca_probing_up = 0;
}

static void BBRStartProbeBW_CRUISE(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->pacing_gain = BBRProbeBwCruisePacingGain;  /* pace at rate */
    bbr_state->cwnd_gain = BBRProbeBwCruiseCwndGain;   /* maintain cwnd */
    bbr_state->state = picoquic_bbr_alg_probe_bw_cruise;
}

static void BBRStartProbeBW_REFILL(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    bbr_state->pacing_gain = BBRProbeBwRefillPacingGain;  /* pace at rate */
    bbr_state->cwnd_gain = BBRProbeBwRefillCwndGain;   /* maintain cwnd */
    BBRResetLowerBounds(bbr_state);
    bbr_state->bw_probe_up_rounds = 0;
    bbr_state->bw_probe_up_acks = 0;
    bbr_state->full_bw = bbr_state->max_bw;
    bbr_state->ack_phase = picoquic_bbr_acks_refilling;
    BBRStartRound(bbr_state, path_x);
    bbr_state->state = picoquic_bbr_alg_probe_bw_refill;
    path_x->is_cca_probing_up = 1;
}

static void BBRStartProbeBW_UP(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t current_time)
{
    bbr_state->nb_rtt_excess = 0;
    bbr_state->pacing_gain = BBRProbeBwUpPacingGain;  /* pace at rate */
    bbr_state->cwnd_gain = BBRProbeBwUpCwndGain;   /* maintain cwnd */
    bbr_state->ack_phase = picoquic_bbr_acks_probe_starting;
    BBRStartRound(bbr_state, path_x);
    bbr_state->cycle_stamp = current_time; /* start wall clock */
    bbr_state->state = picoquic_bbr_alg_probe_bw_up;
    BBRRaiseInflightHiSlope(bbr_state, path_x);
    path_x->is_cca_probing_up = 1;
}

/* The core state machine logic for ProbeBW: */
static void BBRUpdateProbeBWCyclePhase(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    if (!bbr_state->filled_pipe) {
        return;  /* only handling steady-state behavior here */
    }
    BBRAdaptUpperBounds(bbr_state, path_x, rs, current_time);

    switch (bbr_state->state) {
    case picoquic_bbr_alg_probe_bw_down:
        if (BBRCheckTimeToProbeBW(bbr_state, path_x, rs, current_time))
            return; /* already decided state transition */
#ifdef RTTJitterBufferProbe
        if (BBRCheckPathSaturated(bbr_state, path_x, rs)) {
            return;
        }
#endif
        if (BBRCheckTimeToCruise(bbr_state, path_x)) {
            if (15 * bbr_state->max_bw >= 16 * bbr_state->full_bw &&
                rs->ecn_alpha <= BBRExcessiveEcnCE) {
                /* still growing? */
                bbr_state->full_bw = bbr_state->max_bw;    /* record new baseline level */
                bbr_state->full_bw_count = 0;
                bbr_state->probe_probe_bw_quickly = 1;
            }
            else {
                bbr_state->full_bw_count++;
                if (bbr_state->full_bw_count > 3 ||
                    rs->ecn_alpha > BBRExcessiveEcnCE) {
                    bbr_state->probe_probe_bw_quickly = 0;
                    bbr_state->full_bw_count = 0;
                }
            }
            BBRStartProbeBW_CRUISE(bbr_state);
        }
        break;

    case picoquic_bbr_alg_probe_bw_cruise:
#ifdef RTTJitterBufferProbe
        if (BBRCheckPathSaturated(bbr_state, path_x, rs)) {
            return;
        }
#endif
        if (BBRCheckTimeToProbeBW(bbr_state, path_x, rs, current_time))
            return; /* already decided state transition */
        break;

    case picoquic_bbr_alg_probe_bw_refill:
        /* After one round of REFILL, start UP */
        if (bbr_state->round_start) {
            bbr_state->bw_probe_samples = 1;
            BBRStartProbeBW_UP(bbr_state, path_x, current_time);
        }
        break;

    case picoquic_bbr_alg_probe_bw_up:
        if (BBRHasElapsedInPhase(bbr_state, bbr_state->min_rtt, current_time) &&
            bbr_state->min_rtt > PICOQUIC_MINRTT_THRESHOLD &&
            BBRExpTest(bbr_state, do_exit_probeBW_up_on_delay) &&
            (bbr_state->nb_rtt_excess > 0 ||
                path_x->bytes_in_transit > BBRInflightWithBw(bbr_state, path_x, 1.25, bbr_state->max_bw))) {
            BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
        }
        break;

    default:
        /* In non probe BW states, do nothing. */
        return;
    }
    /* Only in probe BW states, if BW > ceiling, enter startup */
    if (bbr_state->bw > bbr_state->bw_probe_ceiling) {
        BBRReEnterStartup(bbr_state, path_x);
    }
}

static void BBREnterProbeBW(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    bbr_state->bw_probe_ceiling = bbr_state->bw + bbr_state->bw / 2;
    BBRStartProbeBW_DOWN(bbr_state, path_x, current_time);
}
/* End of probe BW specific algorithms */

/* Drain specific processes for BBRv3 */
static void BBREnterDrain(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    path_x->is_ssthresh_initialized = 1; /* Picoquic specific: notify transport that the startup phase is complete */
    bbr_state->state = picoquic_bbr_alg_drain;
    bbr_state->pacing_gain = 1.0 / BBRStartupCwndGain;  /* pace slowly */
    bbr_state->cwnd_gain = BBRStartupCwndGain;   /* maintain cwnd */

    path_x->is_cca_probing_up = 0;
}

static void BBRCheckDrain(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    if (bbr_state->state == picoquic_bbr_alg_drain && path_x->bytes_in_transit <= BBRInflight(bbr_state, path_x, 1.0)) {
        BBREnterProbeBW(bbr_state, path_x, current_time);  /* we estimate that the queue is drained */
    }
}
/* End of drain specific algorithms */

/* ==========================================================================
 * BBR-SAT Extension: Cross-orbit handover for LEO/MEO/GEO satellite networks
 *
 * Design principles (from BBR-SAT experiment context doc):
 * - RTT is TRUSTED immediately (geometry is deterministic from ephemeris)
 * - BW is a CEILING only when confidence==EPHEMERIS; trusted 1 RTT when MEASURED
 * - DRAIN trigger: BW_old > BW_new (NOT BDP comparison -- GEO has highest BDP)
 * - DRAIN and REFILL are mutually exclusive (Opus review fix applied)
 *
 * Orbit class constants:
 *   BBR_SAT_ORBIT_LEO = 0  (~50ms RTT, ~50Mbps)
 *   BBR_SAT_ORBIT_MEO = 1  (~160ms RTT, ~30Mbps)
 *   BBR_SAT_ORBIT_GEO = 2  (~580ms RTT, ~10Mbps)
 *
 * bw_confidence values:
 *   0 = BBR_SAT_BW_EPHEMERIS (derived from orbital mechanics, treat as ceiling)
 *   1 = BBR_SAT_BW_MEASURED  (validated by >= 1 full ProbeBW cycle)
 * ========================================================================== */

/*
 * bbr_sat_handover_predicted -- 5-step handover handler.
 *
 * Called when the operator control plane signals HANDOVER_PREDICTED.
 * target_orbit is one of: 0=LEO, 1=MEO, 2=GEO.
 *
 * DRAIN/REFILL fix (Opus review item a):
 *   Steps 3 and 4 are now mutually exclusive.
 *   DRAIN fires only when BW is decreasing.
 *   REFILL fires only when BW is increasing or unchanged.
 *   When DRAIN exits via BBRCheckDrain()->BBREnterProbeBW(), BW discovery
 *   proceeds naturally through ProbeBW_DOWN -> REFILL -> UP.
 */
static void bbr_sat_handover_confirmed(
    picoquic_bbr_state_t* bbr_state,
    picoquic_path_t*      path_x,
    int                   target_orbit,
    uint64_t              current_time)
{
    if (!bbr_state->sat_enabled) return;
    if (target_orbit < 0 || target_orbit >= 3) return;

    /* Convenience pointers */
    __typeof__(bbr_state->sat_orbit_table[0])* target  = &bbr_state->sat_orbit_table[target_orbit];
    __typeof__(bbr_state->sat_orbit_table[0])* current_entry = &bbr_state->sat_orbit_table[bbr_state->sat_current_orbit];

    /* --- Step 1: Save current converged RTT into current orbit entry.
     * IMPORTANT: We do NOT overwrite max_bw_bps with BBR's cruise-phase
     * measurement. BBR in probe_bw_cruise deliberately paces below link BW,
     * so the measured value underestimates provisioned capacity.
     * The max_bw_bps field retains the provisioned/seeded value for the
     * DRAIN trigger comparison. We only update bw_confidence to MEASURED
     * to record that this orbit has been visited.
     */
    if (bbr_state->min_rtt != UINT64_MAX && bbr_state->min_rtt > 0) {
        current_entry->min_rtt_us = bbr_state->min_rtt;
    }
    if (bbr_state->max_bw > 0 && current_entry->valid) {
        /* Only update confidence, not the provisioned BW ceiling */
        current_entry->bw_confidence = 1; /* BBR_SAT_BW_MEASURED */
    } else if (bbr_state->max_bw > 0) {
        /* No provisioned value seeded -- fall back to measured BW */
        current_entry->max_bw_bps    = bbr_state->max_bw;
        current_entry->bw_confidence = 1; /* BBR_SAT_BW_MEASURED */
        current_entry->valid         = 1;
    }

    /* Adaptive CONFIRMED: no-op path for target RTT ≤ BBRLongRttThreshold.
     * BBRv3 handles LEO/MEO transitions natively — any explicit state change
     * here (min_rtt swap, bw_hi update) can cause cwnd contractions or probe
     * interference that slows convergence below B1's natural 1500ms baseline.
     * Full context switch is reserved for GEO-class targets where BBRv3's
     * startup_long_rtt trap requires aggressive intervention. */
    if (target->valid && target->min_rtt_us > 0 &&
        target->min_rtt_us <= BBRLongRttThreshold) {
        bbr_state->sat_current_orbit = target_orbit;
        return;
    }

    /* --- Step 2: Load target min_rtt -- TRUSTED immediately ---
     * Resets both stamp fields to prevent spurious ProbeRTT on next ack.
     * Also resets probe_rtt_round_done (Opus review item c) -- defensive
     * against a ProbeRTT that was in-progress when handover fired.
     */
    if (target->valid && target->min_rtt_us > 0) {
        bbr_state->min_rtt             = target->min_rtt_us;
        bbr_state->min_rtt_stamp       = current_time;  /* reset 10-s expiry */
        bbr_state->probe_rtt_min_delay = target->min_rtt_us;
        bbr_state->probe_rtt_min_stamp = current_time;  /* push ProbeRTT out 5s */
        bbr_state->probe_rtt_round_done = 0;            /* abort any in-progress ProbeRTT */
        bbr_state->probe_rtt_done_stamp  = 0;            /* prevent BBRCheckProbeRTTDone() overriding DRAIN/REFILL state (Opus review fix) */
        /* Also reset the jitter buffer's short-term min/max to the new RTT
         * so that BBRUpdateMinRTT doesn't immediately re-detect the old RTT */
#ifdef RTTJitterBuffer
        BBRResetRTTJitterBuffer(bbr_state, target->min_rtt_us, current_time);
#endif
    }

    /* --- Step 3 + 4: Load max_bw ceiling and decide DRAIN vs REFILL ---
     * DRAIN/REFILL are mutually exclusive (Opus review fix).
     * DRAIN fires only when BW_old > BW_new.
     * REFILL fires only when BW is increasing or unknown.
     *
     * Additional state resets (Opus review item d):
     *   bw_lo     -> UINT64_MAX  (stale loss-based cap from old orbit)
     *   bw_latest -> 0           (stale delivery rate sample)
     *   inflight_hi -> new BDP ceiling (target BW * target RTT)
     *   inflight_lo -> UINT64_MAX (unconstrained, let ProbeBW rediscover)
     *   extra_acked -> 0         (calibrated to old path's ACK pattern)
     */

    /* Compute new BDP ceiling for inflight_hi: bw(bytes/s) * rtt(s) = bytes
     * bw in bps -> bytes/s = bw/8; rtt in us -> s = rtt/1e6
     * inflight_hi = (max_bw_bps / 8) * (min_rtt_us / 1e6)
     *             = max_bw_bps * min_rtt_us / 8000000
     */
    uint64_t new_min_rtt = (target->valid && target->min_rtt_us > 0)
                           ? target->min_rtt_us : bbr_state->min_rtt;

    /* Reset stale orbit-specific state */
    bbr_state->bw_lo           = UINT64_MAX; /* unconstrained */
    bbr_state->bw_latest       = 0;
    bbr_state->inflight_lo     = UINT64_MAX; /* unconstrained */
    bbr_state->extra_acked     = 0;

    /* Reset recovery flags to prevent PTO-triggered BBRReEnterStartup.
     * In-flight packets from the old orbit will experience inflated RTT
     * after the handover, triggering PTO. Without this reset, PTO fires
     * BBROnExitRecovery -> BBRReEnterStartup -> filled_pipe=0 ->
     * startup_long_rtt trap. Clearing these flags prevents that path. */
    bbr_state->is_in_recovery   = 0;
    bbr_state->is_pto_recovery  = 0;
    bbr_state->packet_conservation = 0;
    /* Reset min_rtt stamp to prevent immediate ProbeRTT */
    bbr_state->probe_rtt_done_stamp = 0;

    if (target->valid && target->max_bw_bps > 0) {
        int bw_decreasing = (current_entry->valid &&
                             current_entry->max_bw_bps > target->max_bw_bps);

        /* Set inflight_hi to new orbit BDP ceiling */
        bbr_state->inflight_hi = (target->max_bw_bps * new_min_rtt) / 8000000ULL;
        if (bbr_state->inflight_hi == 0) {
            bbr_state->inflight_hi = UINT64_MAX;
        }

        if (target->bw_confidence == 0) { /* BBR_SAT_BW_EPHEMERIS */
            /* Set bw_hi as ceiling -- do NOT zero MaxBwFilter or max_bw.
             * BBRv3 ProbeBW cycles discover BW well; zeroing forces slow restart.
             * Only min_rtt needs swapping (10s filter); BW adapts via ProbeBW.
             * Enter ProbeBW_DOWN only if current pacing exceeds target ceiling.
             * (Opus Review #4 fix -- replaces memset + unconditional DRAIN) */
            bbr_state->bw_hi = target->max_bw_bps;

            /* Ensure filled_pipe=1 so ProbeBW logic runs */
            if (!bbr_state->filled_pipe) {
                bbr_state->filled_pipe  = 1;
                bbr_state->full_bw      = bbr_state->max_bw;
                bbr_state->full_bw_count = 0;
            }

            /* ProbeBW_DOWN only if pacing exceeds target ceiling */
            {
                double target_rate = (double)target->max_bw_bps * 0.99;
                if (bbr_state->pacing_rate > target_rate) {
                    /* Downward BW: stale MaxBwFilter holds old-orbit samples
                     * (e.g. 30 Mbps for MEO→GEO). BBRv3 would pace at 27 Mbps
                     * while inflight is capped at the new BDP, and the filter
                     * takes 2-3 rounds × new RTT to flush — causing a 1s T90
                     * regression. Zero the filter and seed max_bw from ephemeris
                     * so BBRv3 operates with correct state from the first ACK. */
                    bbr_state->max_bw = target->max_bw_bps;
                    memset(bbr_state->MaxBwFilter, 0, sizeof(bbr_state->MaxBwFilter));
                    bbr_state->bw_probe_ceiling = target->max_bw_bps * 2;
                    BBREnterProbeBW(bbr_state, path_x, current_time);
                    bbr_state->bw_probe_ceiling = target->max_bw_bps * 2;
                }
                /* else: pacing already within target — min_rtt+bw_hi steer BBR */
            }
        } else { /* BBR_SAT_BW_MEASURED */
            bbr_state->bw_hi  = target->max_bw_bps;
            bbr_state->max_bw = target->max_bw_bps;

            if (!bbr_state->filled_pipe) {
                bbr_state->filled_pipe  = 1;
                bbr_state->full_bw      = target->max_bw_bps;
                bbr_state->full_bw_count = 0;
            }

            if (bw_decreasing) {
                bbr_state->bw_probe_ceiling = target->max_bw_bps * 2;
                BBREnterProbeBW(bbr_state, path_x, current_time);
                bbr_state->bw_probe_ceiling = target->max_bw_bps * 2;
            }
            /* BW increasing + MEASURED: normal ProbeBW_UP handles it */
        }

    } else {
        /* No stored entry -- cold start */
        bbr_state->bw_hi  = UINT64_MAX;
        bbr_state->max_bw = 0;
        bbr_state->inflight_hi = UINT64_MAX;
        memset(bbr_state->MaxBwFilter, 0, sizeof(bbr_state->MaxBwFilter));
        BBRStartProbeBW_REFILL(bbr_state, path_x);
    }
    /* --- Step 5: Update orbit pointer.
     * Learning loop is external -- experiment harness calls
     * bbr_sat_update_learned_bw() after convergence detection. */
    bbr_state->sat_current_orbit = target_orbit;

    /* --- Step 6: Handle startup_long_rtt state.
     * When min_rtt > BBRLongRttThreshold (250ms), BBR uses Hystart.
     * After handover we have ephemeris RTT + provisioned BW -- we know
     * enough to skip Hystart and go directly to ProbeBW.
     * Set filled_pipe=1 and seed bdp so BBRExitStartupLongRtt fires.
     * Only applies to BBR-SAT (not B1/B3/B4 baselines). */
    if (bbr_state->sat_enabled &&
        (bbr_state->state == picoquic_bbr_alg_startup_long_rtt ||
         bbr_state->state == picoquic_bbr_alg_startup ||
         bbr_state->state == picoquic_bbr_alg_startup_resume)) {
        /* Seed BDP from target orbit provisioned values */
        uint64_t target_bdp = (target->max_bw_bps / 8) *
                               (new_min_rtt / 1000000ULL);
        if (target_bdp == 0 && target->max_bw_bps > 0 && new_min_rtt > 0) {
            /* Integer overflow protection: compute differently */
            target_bdp = (target->max_bw_bps * new_min_rtt) / 8000000ULL;
        }
        if (target_bdp > 0) {
            bbr_state->bdp_seed = target_bdp;
        }
        /* Mark pipe as filled -- we know capacity from ephemeris */
        bbr_state->filled_pipe  = 1;
        bbr_state->full_bw      = (target->max_bw_bps > 0) ?
                                   target->max_bw_bps : bbr_state->max_bw;
        bbr_state->full_bw_count = 0;
        /* Exit to Drain then ProbeBW */
        BBREnterDrain(bbr_state, path_x);
        BBRCheckDrain(bbr_state, path_x, current_time);
    }
}

/*
 * bbr_sat_update_learned_bw -- called by experiment harness after one full
 * ProbeBW cycle completes on the new orbit (convergence = max_bw stable
 * over 2 consecutive ProbeBW rounds as observed externally via QLOG).
 */
static void __attribute__((unused)) bbr_sat_update_learned_bw(
    picoquic_bbr_state_t* bbr_state,
    int                   orbit,
    uint64_t              observed_bw_bps)
{
    if (!bbr_state->sat_enabled) return;
    if (orbit < 0 || orbit >= 3) return;
    bbr_state->sat_orbit_table[orbit].max_bw_bps    = observed_bw_bps;
    bbr_state->sat_orbit_table[orbit].bw_confidence = 1; /* MEASURED */
    bbr_state->sat_orbit_table[orbit].valid         = 1;
}

/*
 * bbr_sat_init_orbit_table -- initialise the BBR-SAT extension with
 * ephemeris-derived defaults. Call immediately after picoquic_bbr_init()
 * from the test harness, before the connection starts sending.
 *
 * Default values use upload BW (terminal→server, the direction BBR measures).
 * Realistic satellite asymmetry: download >> upload.
 *   LEO [0]: 50 ms RTT,  10 Mbps UL (50 Mbps DL)
 *   MEO [1]: 160 ms RTT, 10 Mbps UL (30 Mbps DL)
 *   GEO [2]: 580 ms RTT,  3 Mbps UL (10 Mbps DL)
 */
void bbr_sat_init_orbit_table_internal(
    picoquic_bbr_state_t* bbr_state,
    int                   initial_orbit)
{
    memset(bbr_state->sat_orbit_table, 0, sizeof(bbr_state->sat_orbit_table));

    bbr_state->sat_orbit_table[0].min_rtt_us      =  50000ULL;     /* LEO 50ms */
    bbr_state->sat_orbit_table[0].max_bw_bps      = 10000000ULL;   /* 10 Mbps UL */
    bbr_state->sat_orbit_table[0].bw_confidence   = 0;             /* EPHEMERIS */
    bbr_state->sat_orbit_table[0].valid           = 1;

    bbr_state->sat_orbit_table[1].min_rtt_us      = 160000ULL;     /* MEO 160ms */
    bbr_state->sat_orbit_table[1].max_bw_bps      = 10000000ULL;   /* 10 Mbps UL */
    bbr_state->sat_orbit_table[1].bw_confidence   = 0;             /* EPHEMERIS */
    bbr_state->sat_orbit_table[1].valid           = 1;

    bbr_state->sat_orbit_table[2].min_rtt_us      = 580000ULL;     /* GEO 580ms */
    bbr_state->sat_orbit_table[2].max_bw_bps      =  3000000ULL;   /*  3 Mbps UL */
    bbr_state->sat_orbit_table[2].bw_confidence   = 0;             /* EPHEMERIS */
    bbr_state->sat_orbit_table[2].valid           = 1;

    bbr_state->sat_current_orbit = initial_orbit;
    bbr_state->sat_enabled       = 1;
}

/* End of BBR-SAT extension */

/* Startup extension to support careful resume */
static void BBRCheckStartupFullBandwidthGeneric(picoquic_bbr_state_t* bbr_state,
    bbr_per_ack_state_t * rs, double threshold)
{
    if (bbr_state->filled_pipe ||
        !bbr_state->round_start || rs->is_app_limited) {
        return;  /* no need to check for a full pipe now */
    }

    if ((double)bbr_state->max_bw >= threshold*((double)bbr_state->full_bw)) {
        /* still growing? */
        bbr_state->full_bw = bbr_state->max_bw;    /* record new baseline level */
        bbr_state->full_bw_count = 0;
        return;
    }
    bbr_state->full_bw_count++; /* another round w/o much growth */
    if (bbr_state->full_bw_count >= 3) {
        bbr_state->filled_pipe = 1;
    }
}

static void BBREnterStartupResume(picoquic_bbr_state_t* bbr_state)
{
    /* This code is called either when the "bdp seed" is set, or
     * upon "Enter Startup"
     */
    bbr_state->state = picoquic_bbr_alg_startup_resume;
    bbr_state->pacing_gain = BBRStartupResumePacingGain;
    bbr_state->cwnd_gain = BBRStartupResumeCwndGain;
}

static void BBRCheckStartupResume(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs)
{
    if (bbr_state->state == picoquic_bbr_alg_startup_resume) {
        BBRCheckStartupHighLoss(bbr_state, path_x, rs);
        if (!bbr_state->filled_pipe && (double)bbr_state->max_bw > BBRStartupResumeIncreaseThreshold * bbr_state->bdp_seed) {
            BBREnterStartup(bbr_state, path_x);
        }
        else {
            BBRCheckStartupFullBandwidthGeneric(bbr_state, rs, BBRStartupResumeIncreaseThreshold);
            if (bbr_state->filled_pipe) {
                if (bbr_state->full_bw_count > 0) {
                    bbr_state->probe_probe_bw_quickly = 1;
                    bbr_state->full_bw_count = 0;
                }
                BBREnterDrain(bbr_state, path_x);
            }
        }
    }
}

/* Startup specific processes for BBRv3 */
static void BBRCheckStartupHighLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, bbr_per_ack_state_t * rs)
{
    /* TODO: no sample code provided */
    /*
    * A second method BBR uses for estimating the bottleneck is full is by looking at sustained packet losses.
    Specifically for a case where the following criteria are all met:
    The connection has been in fast recovery for at least one full round trip.
    The loss rate over the time scale of a single full round trip exceeds BBRLossThresh (2%).
    There are at least BBRStartupFullLossCnt=3 discontiguous sequence ranges lost in that round trip.

    If these criteria are all met, then BBRCheckStartupHighLoss() sets BBR.filled_pipe = true, which will cause exit Startup and enters Drain
    */
    if (IsInflightTooHigh(bbr_state, path_x, rs)) {
        bbr_state->filled_pipe = 1;
    }
}

static void BBRCheckStartupFullBandwidth(picoquic_bbr_state_t* bbr_state,
    bbr_per_ack_state_t * rs)
{
    if (bbr_state->filled_pipe ||
        !bbr_state->round_start || rs->is_app_limited) {
        return;  /* no need to check for a full pipe now */
    }
    /* Using here 5/4 test instead of double 1.25 */
    if (4*bbr_state->max_bw >= 5*bbr_state->full_bw) {
        /* still growing? */
        bbr_state->full_bw = bbr_state->max_bw;    /* record new baseline level */
        bbr_state->full_bw_count = 0;
        if (rs->ecn_frac < 0.2) {
            return;
        }
    }
    bbr_state->full_bw_count++; /* another round w/o much growth */
    if (bbr_state->full_bw_count >= 3 || rs->ecn_frac >= BBRExcessiveEcnCE) {
        bbr_state->filled_pipe = 1;
    }
}

static void BBRCheckStartupDone(picoquic_bbr_state_t* bbr_state,
    picoquic_path_t * path_x, bbr_per_ack_state_t * rs)
{
    if (bbr_state->state == picoquic_bbr_alg_startup) {
        BBRCheckStartupFullBandwidth(bbr_state, rs);
        BBRCheckStartupHighLoss(bbr_state, path_x, rs);
#ifdef RTTJitterBufferStartup
        if (bbr_state->min_rtt > PICOQUIC_MINRTT_THRESHOLD && IsRTTTooHigh(bbr_state)) {
            bbr_state->filled_pipe = 1;
        }
#endif
        if (bbr_state->filled_pipe) {
            bbr_state->probe_probe_bw_quickly = 1;
            bbr_state->full_bw_count = 0;
            BBREnterDrain(bbr_state, path_x);
        }
    }
}

static void BBREnterStartup(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->state = picoquic_bbr_alg_startup;
    bbr_state->pacing_gain = BBRStartupPacingGain;
    bbr_state->cwnd_gain = BBRStartupCwndGain;
    path_x->is_cca_probing_up = 1;
}

static void BBRReEnterStartup(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->full_bw = 0;
    bbr_state->filled_pipe = 0;
    bbr_state->full_bw_count = 0;
    bbr_state->probe_probe_bw_quickly = 1;
    BBREnterStartup(bbr_state, path_x);
}

/* End of BBRv3 startup specific */

/* Startup long RTT -- in that state, the code uses Hystart rather than BBR Startup */
void BBREnterStartupLongRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    bbr_state->state = picoquic_bbr_alg_startup_long_rtt;

    if (path_x->rtt_min > PICOQUIC_TARGET_RENO_RTT) {
        if (path_x->rtt_min > PICOQUIC_TARGET_SATELLITE_RTT) {
            cwnd = (uint64_t)((double)cwnd * (double)PICOQUIC_TARGET_SATELLITE_RTT / (double)PICOQUIC_TARGET_RENO_RTT);
        }
        else {
            cwnd = (uint64_t)((double)cwnd * (double)path_x->rtt_min / (double)PICOQUIC_TARGET_RENO_RTT);
        }
    }
    if (cwnd < bbr_state->bdp_seed) {
        cwnd = bbr_state->bdp_seed;
    }
    if (cwnd > path_x->cwin) {
        path_x->cwin = cwnd;
    }
    path_x->is_cca_probing_up = 1;
}

static void BBRExitStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    /* Reset the round filter so it will start at current time */
    BBRStartRound(bbr_state, path_x);
    bbr_state->round_count++;
    bbr_state->rounds_since_probe++;
    bbr_state->round_start = 1;
    /* Set the filled pipe indicator */
    bbr_state->filled_pipe = 1;
    /* Check the RTT measurement for pathological cases */
    if ((bbr_state->rtt_filter.is_init || bbr_state->rtt_filter.sample_current > 0) &&
        bbr_state->min_rtt > 30000000 &&
        bbr_state->rtt_filter.sample_max < bbr_state->min_rtt) {
        bbr_state->min_rtt = bbr_state->rtt_filter.sample_max;
        bbr_state->min_rtt_stamp = current_time;
    }
#ifdef RTTJitterBuffer_maybe
    BBRResetRTTJitterBuffer(bbr_state, bbr_state->min_rtt, current_time);
#endif
    /* Enter drain */
    BBREnterDrain(bbr_state, path_x);
    /* If there were just few bytes in transit, enter probe */
    BBRCheckDrain(bbr_state, path_x, current_time);
}

void BBRCheckStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs, uint64_t current_time)
{
    if ((bbr_state->state == picoquic_bbr_alg_startup ||
        bbr_state->state == picoquic_bbr_alg_startup_resume) &&
        path_x->rtt_min > BBRLongRttThreshold) {
        BBREnterStartupLongRTT(bbr_state, path_x);
    }
    else if (bbr_state->state != picoquic_bbr_alg_startup_long_rtt) {
        return;
    }

    if (picoquic_cc_hystart_test(&bbr_state->rtt_filter, rs->rtt_sample,
        path_x->pacing.packet_time_microsec, current_time, 0)) {
        BBRExitStartupLongRtt(bbr_state, path_x, current_time);
    }
    else if (rs->ecn_alpha > BBRExcessiveEcnCE) {
        BBRExitStartupLongRtt(bbr_state, path_x, current_time);
    }
    else {
        int excessive_loss = picoquic_cc_hystart_loss_volume_test(&bbr_state->rtt_filter, picoquic_congestion_notification_repeat,
            rs->newly_acked, rs->newly_lost);
        if (excessive_loss) {
            BBRExitStartupLongRtt(bbr_state, path_x, current_time);
        }
    }
}

void BBRUpdateStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs)
{
    if (path_x->last_time_acked_data_frame_sent > path_x->last_sender_limited_time) {
        path_x->cwin += picoquic_cc_slow_start_increase(path_x, rs->newly_acked);
    }

    uint64_t max_win = PICOQUIC_BYTES_FROM_RATE(bbr_state->min_rtt, path_x->peak_bandwidth_estimate);
    if (max_win < bbr_state->bdp_seed) {
        max_win = bbr_state->bdp_seed;
    }

    uint64_t min_win = max_win /= 2;

    if (path_x->cwin < min_win) {
        path_x->cwin = min_win;
    }
}

void BBRSetBdpSeed(picoquic_bbr_state_t* bbr_state, uint64_t bdp_seed)
{
    bbr_state->bdp_seed = bdp_seed;
    if (bbr_state->state == picoquic_bbr_alg_startup &&
        bbr_state->bdp_seed > bbr_state->max_bw) {
        BBREnterStartupResume(bbr_state);
    }
}

/* BBRv3 per loss steps.
* TODO: this is part of "path" model
 */

#if 0
/* At what prefix of packet did losses exceed BBRLossThresh? */
static uint64_t BBRInflightHiFromLostPacket(bbr_per_ack_state_t * rs, picoquic_per_ack_state_t* packet_state)
{
    uint64_t packet_size = packet_state->nb_bytes_newly_lost;
    /* What was in flight before this packet? */
    uint64_t inflight_prev = rs->tx_in_flight - packet_size;
    /* What was lost before this packet? */
    uint64_t lost_prev = rs->lost - packet_size;
    double lost_prefix = (BBRLossThresh *((double)(inflight_prev - lost_prev))) /
        (1.0 - BBRLossThresh);
    /* At what inflight value did losses cross BBRLossThresh? */
    uint64_t inflight = inflight_prev + (uint64_t)lost_prefix;
    return inflight;
}
#endif
#if 1
/* TODO: reconcile this direct handling of path->cwin with the handling
* of the "inflight too high" variable. 
 */
static void BBRUpdateRecoveryOnLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t newly_lost)
{
    if (path_x->nb_retransmit >= 1 && bbr_state->is_in_recovery && bbr_state->is_pto_recovery) {
        if (path_x->cwin > newly_lost) {
            path_x->cwin -= newly_lost;
            if (path_x->cwin < 2 * path_x->send_mtu) {
                path_x->cwin = 2 * path_x->send_mtu;
            }
        }
    }
}
#else
/* This is the handling specified in the draft.
* It is mostly not needed, because:
* 
* - timeout losses are covered by "BBROnRTO",
* - no timeout losses are handled as part of ACK processing.
 */
static void BBRHandleLostPacket(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, picoquic_per_ack_state_t* packet_state, uint64_t current_time)
{
    if (bbr_state->bw_probe_samples == 0) {
        return; /* not a packet sent while probing bandwidth */
    }
    bbr_per_ack_state_t rs = { 0 };
    BBRSetRsFromAckState(path_x, packet_state, &rs);

    if (IsInflightTooHigh(bbr_state, path_x, &rs)) {
        rs.tx_in_flight = BBRInflightHiFromLostPacket(&rs, packet_state);
        BBRHandleInflightTooHigh(bbr_state, path_x, &rs, current_time);
    }
}

static void BBRUpdateOnLoss(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, picoquic_per_ack_state_t* packet_state, uint64_t current_time)
{
    BBRHandleLostPacket(bbr_state, path_x, packet_state, current_time);
}
#endif

/* ECN related functions */
static picoquic_packet_context_t* BBRAccessEcnPacketContext(picoquic_path_t* path_x)
{
    /* TODO: ECN counts should be a function of path, not number space! */
    picoquic_packet_context_t* pkt_ctx = &path_x->cnx->pkt_ctx[picoquic_packet_context_application];

    if (path_x->cnx->is_multipath_enabled) {
        pkt_ctx = &path_x->pkt_ctx;
    }
    else if (path_x != path_x->cnx->path[0]) {
        /* When doing simple multipath, or when preparing transitions,
         * only consider the default path */
        pkt_ctx = NULL;
    }

    return pkt_ctx;
}

static void BBRComputeEcnFrac(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    picoquic_packet_context_t* pkt_ctx = BBRAccessEcnPacketContext(path_x);
    uint64_t delta_ect1 = 0;
    uint64_t delta_ce = 0;
    rs->ecn_frac = 0.0;

    if (pkt_ctx != NULL &&
        pkt_ctx->ecn_ect1_total_remote >= bbr_state->ecn_ect1_last_round &&
        pkt_ctx->ecn_ce_total_remote >= bbr_state->ecn_ce_last_round) {
        if (pkt_ctx->ecn_ect1_total_remote == 0) {
            /* Probably legacy ECN -- treat it the same way we would treat proportional ECN */
            delta_ect1 = (rs->delivered/path_x->send_mtu);
        }
        else {
            delta_ect1 = pkt_ctx->ecn_ect1_total_remote - bbr_state->ecn_ect1_last_round;
            delta_ce = pkt_ctx->ecn_ce_total_remote - bbr_state->ecn_ce_last_round;
        }
        if (delta_ect1 + delta_ce > 0) {
            rs->ecn_ce = delta_ce;
            rs->ecn_frac = (double)delta_ce / (double)(delta_ect1 + delta_ce);
            rs->ecn_alpha = (rs->ecn_frac + 15.0 * bbr_state->ecn_alpha) / 16.0;
        }
    }
}

static void BBRAdvanceEcnFrac(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t* rs)
{
    if (bbr_state->round_start) {
        picoquic_packet_context_t* pkt_ctx = BBRAccessEcnPacketContext(path_x);

        if (pkt_ctx != NULL) {
            if (pkt_ctx->ecn_ect1_total_remote < bbr_state->ecn_ect1_last_round ||
                pkt_ctx->ecn_ce_total_remote < bbr_state->ecn_ce_last_round) {
                bbr_state->ecn_alpha = 0;
            }
            else {
                bbr_state->ecn_alpha = (rs->ecn_frac + 15.0 * bbr_state->ecn_alpha) / 16;
            }
            bbr_state->ecn_ect1_last_round = pkt_ctx->ecn_ect1_total_remote;
            bbr_state->ecn_ce_last_round = pkt_ctx->ecn_ce_total_remote;
        }
    }
}

/* BBRv3 per ACK steps
* The function BBRUpdateOnACK is executed for each ACK notification on the API 
*/
static void BBRUpdateModelAndState(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRUpdateLatestDeliverySignals(bbr_state, path_x, rs);
    BBRUpdateCongestionSignals(bbr_state, path_x, rs);
    BBRUpdateACKAggregation(bbr_state, path_x, rs, current_time);
    BBRCheckStartupLongRtt(bbr_state, path_x, rs, current_time);
    BBRCheckStartupResume(bbr_state, path_x, rs);
    BBRCheckStartupDone(bbr_state, path_x, rs);
    BBRCheckRecovery(bbr_state, path_x, rs, current_time);
    BBRCheckDrain(bbr_state, path_x, current_time);
    BBRUpdateProbeBWCyclePhase(bbr_state, path_x, rs, current_time);
    BBRUpdateMinRTT(bbr_state, path_x, rs, current_time);
    BBRCheckProbeRTT(bbr_state, path_x, rs, current_time);
    BBRAdvanceLatestDeliverySignals(bbr_state, rs);
    BBRAdvanceEcnFrac(bbr_state, path_x, rs);
    BBRBoundBWForModel(bbr_state);
}

static void BBRUpdateControlParameters(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs)
{
    BBRSetPacingRate(bbr_state);
    BBRSetSendQuantum(bbr_state, path_x);
    BBRSetCwnd(bbr_state, path_x, rs);
}

void  BBRUpdateOnACK(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, bbr_per_ack_state_t * rs, uint64_t current_time)
{
    BBRUpdateModelAndState(bbr_state, path_x, rs, current_time);
    if (bbr_state->state == picoquic_bbr_alg_startup_long_rtt) {
        BBRUpdateStartupLongRtt(bbr_state, path_x, rs);
    }
    else {
        BBRUpdateControlParameters(bbr_state, path_x, rs);
    }
}

/* First step of BBR ACK processing:
* convert the discrete arguments of "picoquic_bbr_notify"
* into the "rs" structure used in the BBRv3 draft. We do
* that so that it is easy to compare the code to the draft.
* 
* 
 * Code maintains the following counters per path
 * - total_bytes_lost -- number of bytes deemed lost from beginning of path
 * - delivered -- amount delivered so far
 * - rtt_sample -- last rtt sample
 * - bytes_in_transit -- bytes currently in flight
 * 
 * It does not contain `data_lost`, but that could be inferred if
 * we keep a variable `nb_bytes_lost_since_packet_sent`.
 * The packet data contains: delivered_prior, so that the BBR variable
 * "delivered" can be computed = path->delivered - packet->delivered_prior.
 */
static void BBRSetRsFromAckState(picoquic_path_t* path_x, picoquic_per_ack_state_t* ack_state, bbr_per_ack_state_t* rs)
{
    /* Need to compute the delivery rate */
    if (path_x->bandwidth_estimate > 0) {
        rs->delivery_rate = path_x->bandwidth_estimate;
    }
    else if (ack_state->rtt_measurement > 0) {
        rs->delivery_rate = PICOQUIC_RATE_FROM_BYTES(ack_state->nb_bytes_delivered_since_packet_sent, ack_state->rtt_measurement);
    }
    else
    {
        rs->delivery_rate = 40000;
    }
    rs->delivered = ack_state->nb_bytes_delivered_since_packet_sent;
    /* variable in path */
    rs->rtt_sample = path_x->rtt_sample;
    /* variables from call */
    rs->newly_acked = ack_state->nb_bytes_acknowledged; /* volume of data acked by current ack */
    rs->newly_lost = ack_state->nb_bytes_newly_lost; /* volume of data marked lost on ack received */
    rs->lost = ack_state->nb_bytes_lost_since_packet_sent;
    rs->tx_in_flight = ack_state->inflight_prior;
    rs->is_app_limited = ack_state->is_app_limited; /*Checked that this is properly implemented */   
    rs->is_cwnd_limited = ack_state->is_cwnd_limited;
}

static void picoquic_bbr_notify_ack(
    picoquic_bbr_state_t* bbr_state,
    picoquic_path_t* path_x,
    picoquic_per_ack_state_t* ack_state,
    uint64_t current_time)
{
    bbr_per_ack_state_t rs = { 0 };
    BBRSetRsFromAckState(path_x, ack_state, &rs);
    BBRComputeEcnFrac(bbr_state, path_x, &rs);
    BBRUpdateOnACK(bbr_state, path_x, &rs, current_time);
}

/*
 * In order to implement BBR, we map generic congestion notification
 * signals to the corresponding BBR actions.
 */
static void picoquic_bbr_notify(
    picoquic_cnx_t* UNUSED(cnx),
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    picoquic_per_ack_state_t * ack_state,
    uint64_t current_time)
{
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)path_x->congestion_alg_state;
    path_x->is_cc_data_updated = 1;

    if (bbr_state != NULL) {
        switch (notification) {
        case picoquic_congestion_notification_ecn_ec:
            /* TODO */
            break;
        case picoquic_congestion_notification_repeat:
            BBRUpdateRecoveryOnLoss(bbr_state, path_x, ack_state->nb_bytes_newly_lost);
            break;
        case picoquic_congestion_notification_timeout:
            BBRExitLostFeedback(bbr_state, path_x);
            /* if loss is PTO, we should start the OnPto processing */
            BBROnEnterRTO(bbr_state, path_x, ack_state->lost_packet_number);
            break;
        case picoquic_congestion_notification_spurious_repeat:
            /* handling of suspension */
            BBROnSpuriousLoss(bbr_state, path_x, ack_state->lost_packet_number, current_time);
            break;
        case picoquic_congestion_notification_lost_feedback:
            /* Feedback has been lost. It will be restored at the next notification. */
            BBRExpGate(bbr_state, do_control_lost, break);
            BBREnterLostFeedback(bbr_state, path_x);
            break;
        case picoquic_congestion_notification_rtt_measurement:
            /* TODO: this call is subsumed by the acknowledgement notification.
             * Consider removing it from the API once other CC algorithms are updated.  */
            break;
        case picoquic_congestion_notification_acknowledgement:
            BBRExitLostFeedback(bbr_state, path_x);
            picoquic_bbr_notify_ack(bbr_state, path_x, ack_state, current_time);
            if (bbr_state->state == picoquic_bbr_alg_startup_long_rtt) {
                picoquic_update_pacing_data(path_x, 1);
            }
            else if (bbr_state->pacing_rate > 0) {
                /* Set the pacing rate in picoquic sender */
                picoquic_update_pacing_rate(path_x, bbr_state->pacing_rate, bbr_state->send_quantum);
            }
            break;
        case picoquic_congestion_notification_cwin_blocked:
            break;
        case picoquic_congestion_notification_reset:
            picoquic_bbr_reset(bbr_state, path_x, current_time);
            break;
        case picoquic_congestion_notification_seed_cwin:
            BBRSetBdpSeed(bbr_state, ack_state->nb_bytes_acknowledged);
            break;
        case picoquic_congestion_notification_satellite_handover:
            /* nb_bytes_acknowledged carries signal type:
             *   0-2:  BBR-SAT handover (orbit class LEO/MEO/GEO)
             *   0x10: B3 cwnd-freeze
             *   0x11: B3 cwnd-unfreeze
             *   0x20: B4 send-pause
             *   0x21: B4 send-resume
             */
            {
                uint64_t sig = ack_state->nb_bytes_acknowledged;
                if (sig < 3) {
                    /* CONFIRMED: full BDP context switch */
                    bbr_sat_handover_confirmed(bbr_state, path_x,
                        (int)sig, current_time);
                } else if (sig >= 0x30 && sig < 0x33) {
                    /* PREDICTED: queue drain only, no context switch */
                    bbr_sat_handover_phase1(bbr_state, path_x, current_time);
                } else if (sig == 0x10) {
                    bbr_sat_cwnd_freeze(path_x->cnx, path_x);
                } else if (sig == 0x11) {
                    bbr_sat_cwnd_unfreeze(path_x->cnx);
                } else if (sig == 0x20) {
                    bbr_sat_send_pause(path_x->cnx);
                } else if (sig == 0x21) {
                    bbr_sat_send_resume(path_x->cnx);
                }
            }
            break;
        default:
            /* ignore */
            break;
        }
    }
}

/* Observe the state of congestion control */

void picoquic_bbr_observe(picoquic_path_t* path_x, uint64_t* cc_state, uint64_t* cc_param)
{
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)path_x->congestion_alg_state;
    *cc_state = (uint64_t)bbr_state->state;
    *cc_param = bbr_state->bw;
}

#define picoquic_bbr_ID "bbr" /* BBR */

picoquic_congestion_algorithm_t picoquic_bbr_algorithm_struct = {
    picoquic_bbr_ID, PICOQUIC_CC_ALGO_NUMBER_BBR, PICOQUIC_ECN_ECT_0,
    picoquic_bbr_init,
    picoquic_bbr_notify,
    picoquic_bbr_delete,
    picoquic_bbr_observe
};

picoquic_congestion_algorithm_t* picoquic_bbr_algorithm = &picoquic_bbr_algorithm_struct;
/* ==========================================================================
 * BBR-SAT public accessor functions -- append to END of bbr.c
 * (after the existing picoquic_bbr_observe function, before EOF)
 *
 * These provide an opaque API so test code does not need to include
 * the private picoquic_bbr_state_t struct definition.
 * ========================================================================== */

/* Helper: get BBR state from connection path 0 */
static picoquic_bbr_state_t* bbr_sat_get_state_ptr(picoquic_cnx_t* cnx)
{
    if (cnx == NULL || cnx->path[0] == NULL) return NULL;
    return (picoquic_bbr_state_t*)cnx->path[0]->congestion_alg_state;
}

/* Initialise BBR-SAT orbit table (public API wrapping static bbr_sat_init_orbit_table) */
void bbr_sat_init_orbit_table(picoquic_cnx_t* cnx, int initial_orbit)
{
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    if (s == NULL) return;

    memset(s->sat_orbit_table, 0, sizeof(s->sat_orbit_table));

    s->sat_orbit_table[BBR_SAT_ORBIT_LEO].min_rtt_us    =  50000ULL;
    s->sat_orbit_table[BBR_SAT_ORBIT_LEO].max_bw_bps    = 50000000ULL;
    s->sat_orbit_table[BBR_SAT_ORBIT_LEO].bw_confidence = 0;
    s->sat_orbit_table[BBR_SAT_ORBIT_LEO].valid         = 1;

    s->sat_orbit_table[BBR_SAT_ORBIT_MEO].min_rtt_us    = 160000ULL;
    s->sat_orbit_table[BBR_SAT_ORBIT_MEO].max_bw_bps    = 30000000ULL;
    s->sat_orbit_table[BBR_SAT_ORBIT_MEO].bw_confidence = 0;
    s->sat_orbit_table[BBR_SAT_ORBIT_MEO].valid         = 1;

    s->sat_orbit_table[BBR_SAT_ORBIT_GEO].min_rtt_us    = 580000ULL;
    s->sat_orbit_table[BBR_SAT_ORBIT_GEO].max_bw_bps    = 10000000ULL;
    s->sat_orbit_table[BBR_SAT_ORBIT_GEO].bw_confidence = 0;
    s->sat_orbit_table[BBR_SAT_ORBIT_GEO].valid         = 1;

    s->sat_current_orbit = initial_orbit;
    s->sat_enabled       = 1;
}

/* Accessors */
uint64_t bbr_sat_get_min_rtt(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? s->min_rtt : UINT64_MAX;
}
uint64_t bbr_sat_get_max_bw(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? s->max_bw : 0;
}
int bbr_sat_get_state(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? (int)s->state : -1;
}
int bbr_sat_get_current_orbit(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? s->sat_current_orbit : -1;
}
int bbr_sat_get_filled_pipe(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? (int)s->filled_pipe : 0;
}
uint64_t bbr_sat_get_bw_lo(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? s->bw_lo : 0;
}
uint64_t bbr_sat_get_inflight_lo(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? s->inflight_lo : 0;
}
int bbr_sat_get_probe_rtt_round_done(picoquic_cnx_t* cnx) {
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    return s ? (int)s->probe_rtt_round_done : 0;
}
/* ==========================================================================
 * B3: cwnd-freeze baseline (StarQUIC-style)
 * Freeze cwnd and pacing on handover signal, unfreeze on resume signal.
 * ========================================================================== */
void bbr_sat_cwnd_freeze(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
{
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    if (s == NULL) return;
    s->sat_cwnd_frozen        = 1;
    s->sat_frozen_cwnd        = path_x->cwin;
    s->sat_frozen_pacing_rate = s->pacing_rate;
}

void bbr_sat_cwnd_unfreeze(picoquic_cnx_t* cnx)
{
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    if (s == NULL) return;
    s->sat_cwnd_frozen = 0;
    /* pacing_rate and cwin will be updated on next ACK via normal BBR path */
}

/* ==========================================================================
 * B4: pause/resume baseline (Creo-style)
 * Completely stop sending on handover signal, resume on resume signal.
 * ========================================================================== */
void bbr_sat_send_pause(picoquic_cnx_t* cnx)
{
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    if (s == NULL) return;
    s->sat_send_paused = 1;
}

void bbr_sat_send_resume(picoquic_cnx_t* cnx)
{
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    if (s == NULL) return;
    s->sat_send_paused = 0;
    /* BBR will re-enter startup to re-probe BW after pause */
    BBRReEnterStartup(s, cnx->path[0]);
}

/* End of BBR-SAT public accessor functions */

/* bbr_sat_seed_orbit_bw -- public API to seed a specific orbit's BW.
 * Used by test harness to set provisioned BW before handover fires,
 * ensuring DRAIN condition compares against actual link BW not ephemeris. */
void bbr_sat_seed_orbit_bw(picoquic_cnx_t* cnx, int orbit, uint64_t bw_bps)
{
    picoquic_bbr_state_t* s = bbr_sat_get_state_ptr(cnx);
    if (s == NULL || orbit < 0 || orbit >= 3) return;
    s->sat_orbit_table[orbit].max_bw_bps    = bw_bps;
    s->sat_orbit_table[orbit].bw_confidence = 1; /* MEASURED */
    s->sat_orbit_table[orbit].valid         = 1;
}
