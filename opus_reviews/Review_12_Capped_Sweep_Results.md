# BBR-SAT Review #12 — Capped Sweep Results & Paper Restructure

**Date:** 2026-05-18  
**Reviewer request:** Full review of paper after capped-buffer primary sweep and paper restructure.

---

## What Changed Since Review #11

Review #11 had three reject reasons. Here is what was done:

**Reject Reason 1 (Uncapped queue as primary): FIXED**

- Ran a full 5400-job sweep (5 baselines × 6 transitions × 3 T_HO × 6 lead times × 10 runs) with `use_bdp_cap=1` (1×BDP droptail, `queue_delay_max = 2 × one-way latency`).
- Restructured paper: 1×BDP cap is now the primary evaluation model. Uncapped results relegated to a "Large-buffer reference" paragraph.
- All tables and narrative updated to lead with capped results.

**Reject Reason 2 (LEO→MEO N/C as real pathology): PARTIALLY FIXED**

- Implemented the light-touch min_rtt swap: on the CONFIRMED path for LEO/MEO targets (RTT ≤ 250ms), BBR-SAT now immediately updates `min_rtt`, `min_rtt_stamp`, `probe_rtt_min_delay`, `probe_rtt_min_stamp`, and `sat_current_orbit`, then returns without touching MaxBwFilter, bw_hi, or entering REFILL.
- Smoke tests and capped sweep confirm BBR-SAT LEO→MEO is STILL N/C (7.12 Mbps steady-state), same as B1. Root cause is fundamental: BBR's MaxBwFilter plateaus at ~71% utilization on a 10 Mbps link with 150ms RTT (window too small to probe past the plateau). CUBIC overcomes this at 5.5s via loss-driven growth.
- Paper now describes this as a genuine BBRv3 architecture limitation (not merely a metric artifact), while noting the 10 Mbps capacity is unchanged and CUBIC's 5.5s convergence is via packet loss.

**Reject Reason 3 (CUBIC dominates): RESOLVED by capped buffer**

Under 1×BDP cap: BBR-SAT=4.5s/99% util vs CUBIC=5.5s/97%. BBR-SAT wins on both T90 and zero-loss operation.

**Also fixed (not in Review #11, found in sweep):**

- B4 (send-pause/resume) is catastrophically broken for GEO→LEO under the cap: N/C, 0.33 MB goodput, 45 MB peak queue (a retransmit storm from zero-duration pause). Added to paper.
- Corrected link model (DL/UL split): LEO=50/10, MEO=30/10, GEO=10/3 Mbps. BBR measures upload (c_to_s) bottleneck. Orbit table seeded with upload BW.

---

## Capped Sweep Primary Results

### Table: T_HO=30s, lead=0, no loss, 1×BDP droptail cap

| Baseline | LEO→MEO | LEO→GEO | MEO→LEO | MEO→GEO | GEO→LEO | GEO→MEO |
|----------|---------|---------|---------|---------|---------|---------|
| B1       | N/C‡    | N/C     | 0.5s    | 0.5s    | 1.5s    | 2.5s    |
| B3       | N/C‡    | N/C     | 0.5s    | 0.5s    | 1.5s    | 2.5s    |
| B4       | N/C‡    | N/C     | 0.5s    | 0.5s    | **N/C§**| 1.5s    |
| BBR-SAT  | N/C‡    | **4.5s**| 0.5s    | 0.5s    | 1.5s    | 2.5s    |
| CUBIC    | **5.5s**| **5.5s**| 0.5s    | 0.5s    | 1.5s    | 1.5s    |

‡ Metric artifact: LEO/MEO share 10 Mbps upload; T90 threshold (9 Mbps) unreachable due to BBR MaxBwFilter plateau at 7.12 Mbps.  
§ B4 catastrophic: retransmit storm from zero-duration pause; 45 MB peak queue, 0.33 MB goodput.

### LEO→GEO Detail (primary transition)

| Baseline | T90   | Util | Goodput  | Peak Q  |
|----------|-------|------|----------|---------|
| B1       | N/C   | 69%  | 13.7 MB  | 162 KB  |
| B3       | N/C   | 69%  | 13.7 MB  | 162 KB  |
| B4       | N/C   | 71%  | 14.0 MB  | 164 KB  |
| BBR-SAT  | **4.5s** | **99%** | **20.1 MB** | 428 KB |
| CUBIC    | 5.5s  | 97%  | 19.6 MB  | 614 KB  |

BBR-SAT: 46% more goodput than B1; 18% faster T90 than CUBIC; zero packet loss.

### BBR-SAT LEO→GEO Lead-Time Sweep (1×BDP cap)

| T_HO | lead=0 | lead=2 | lead=5 | lead=10 | lead=20 | lead=30 |
|------|--------|--------|--------|---------|---------|---------|
| 30s  | 4.5s   | **1.5s** | 4.5s | 4.5s   | 4.5s    | 2.5s    |
| 60s  | 1.5s   | 1.5s   | 1.5s   | 1.5s    | 1.5s    | 1.5s    |
| 120s | 4.5s   | **1.5s** | 1.5s | 1.5s   | 1.5s    | 4.5s    |

The effective lead-time window is **ℓ ∈ [0, 2] s** for T_HO=30s. At T_HO=60s, zero lead time gives 1.5s (longer pre-handover interval allows more probe cycles). At T_HO=120s, the orbit table's 10s `min_rtt` expiry makes the seeded RTT stale by handover; a 2s lead time re-seeds it just in time.

### Large-Buffer Reference (uncapped, for comparison)

| Baseline | LEO→GEO T90 | Goodput | Peak Q    |
|----------|-------------|---------|-----------|
| B1       | 12.5s†      | 2.3 MB  | 12,544 KB |
| B3       | 12.5s†      | 2.3 MB  | 12,544 KB |
| B4       | N/C         | 1.9 MB  | 12,329 KB |
| BBR-SAT  | 2.5s        | 18.6 MB | **286 KB** |
| CUBIC    | 10.5s       | 19.2 MB | 307 KB    |

† Transient only; throughput collapses after one window. 44× queue reduction for BBR-SAT vs B1.

---

## Potential Issues for Review

### Issue 1: BBR-SAT MEO→GEO Goodput Regression

BBR-SAT MEO→GEO has the same T90 (0.5s) as B1 but **significantly less goodput** (17.95 MB vs 20.82 MB, a 14% regression). B4 GEO→MEO also shows a high peak queue (953 KB).

**Root cause:** MEO→GEO is a downward BW transition with GEO target (RTT=580ms > BBRLongRttThreshold). The adaptive CONFIRMED handler triggers a full context switch: MaxBwFilter zeroed, max_bw seeded at 3 Mbps, pacing drops from 10 Mbps. During this reset+DRAIN phase, throughput dips for ~1-2 seconds, reducing total goodput even though T90 (time to reach 2.7 Mbps sustained) is 0.5s.

**Concern:** The paper text says "BBR-SAT's CONFIRMED handler is a no-op (target RTT ≤ 250ms for MEO/LEO)" for moderate transitions. This is incorrect for MEO→GEO (GEO target, RTT=580ms triggers full switch). The no-op claim only applies to MEO→LEO, GEO→LEO, and GEO→MEO.

**Questions for reviewer:**
- Should we suppress the full context switch for MEO→GEO (since it recovers quickly anyway) to avoid the goodput regression?
- Or is 14% goodput regression acceptable given T90 stays at 0.5s?
- Does the paper need to explicitly call out the MEO→GEO goodput regression?

### Issue 2: LEO→MEO N/C is a BBRv3 Architecture Limit, Not BBR-SAT Failure

The MaxBwFilter cap at 71% utilization (7.12 Mbps steady-state on a 10 Mbps link with 150ms RTT) affects ALL BBRv3-based variants equally. CUBIC's loss-driven mechanism overcomes it.

**Current paper treatment:** Described as "metric artifact" plus "genuine BBRv3 limitation." Is this framing strong enough? Reviewers may push back that BBR-SAT should fix this if it has orbit awareness.

**Counter-argument:** Fixing the MaxBwFilter plateau for LEO→MEO would require artificially inflating max_bw or forcing ProbeBW_UP — both of which could destabilize other transitions. The light-touch min_rtt swap (already implemented) is the correct fix for min_rtt staleness, but it doesn't help here because the bottleneck is MaxBwFilter, not min_rtt.

### Issue 3: Lead-Time Figure Needs Updating

The current `fig_t90_lead.pdf` shows **uncapped** results (flat 2.5s line for BBR-SAT). Under the capped buffer, the lead-time behavior is different (4.5s at most lead times, 1.5s at ℓ=2). The figure caption has been updated to note "uncapped queue shown for reference," but the figure itself still shows uncapped data.

**Question:** Should we regenerate the figure with capped buffer data? The capped lead-time profile is more complex (non-monotonic: 4.5→1.5→4.5→...→2.5) and may require a different presentation.

### Issue 4: Abstract Claim Precision

The abstract now says "converges to 90% of new-orbit capacity in 4.5 s for the critical LEO→GEO transition under a realistic 1×BDP droptail buffer cap." This is accurate for T_HO=30s, ℓ=0.

But with ℓ=2s, T90=1.5s. Should the abstract say "as fast as 1.5s with a 2-second advance signal"?

---

## Current Paper Sections (Abbreviated for Review)

### Abstract (current)

> Low-Earth orbit (LEO), medium-Earth orbit (MEO), and geostationary (GEO) satellite constellations are increasingly deployed together, creating multi-orbit networks in which a single QUIC connection may traverse orbit-class boundaries mid-flight. Such handovers impose abrupt, step-change shifts in available bandwidth (up to 3.3× upload reduction) and round-trip time (up to 12× increase), which existing congestion-control algorithms handle poorly: BBRv3 stalls for the duration of its two-cycle bandwidth filter, and simple heuristics such as congestion-window freeze or send-pause succeed only at a single, carefully tuned lead time. We present **BBR-SAT**, a minimal extension of BBRv3 that equips the sender with an orbit parameter table seeded from ephemeris data and a two-phase handover protocol: a PREDICTED signal initiates a proactive queue drain via ProbeBW_DOWN, and a CONFIRMED signal adaptively resets the BDP context from the orbit table for downward transitions (GEO target) while acting as a no-op for upward transitions that self-correct naturally. Implemented in picoquic and evaluated in a zero-loss shared-link simulator across all six pairwise LEO/MEO/GEO transitions at three handover times and six advance lead times, BBR-SAT converges to 90% of new-orbit capacity in 4.5 s for the critical LEO→GEO transition under a realistic 1×BDP droptail buffer cap — the only zero-loss algorithm to converge; CUBIC converges in 5.5 s via packet loss, while all BBR baselines fail (N/C, ≤71% utilisation). In the uncapped large-buffer scenario, BBR-SAT reduces peak queue by 44× over vanilla BBRv3. Fairness analysis shows that BBR-SAT co-exists equitably with competing BBRv3 flows (Jain J = 0.994) and neither worsens nor repairs the pre-existing BBRv3 deference to CUBIC.

### §V-A.2 LEO→GEO: the Critical Transition (current, abbreviated)

Primary result under 1×BDP cap:
- B1/B3: N/C, 69% util, 13.7 MB goodput (pacing-rate architecture prevents cwnd-based drain)
- B4: N/C, 71% util, 14.0 MB goodput (narrow timing window not satisfiable)
- BBR-SAT: **4.5s, 99% util, 20.1 MB goodput, zero loss** (46% more than B1)
- CUBIC: 5.5s, 97% util, 19.6 MB goodput (18% slower than BBR-SAT, via loss)

Large-buffer reference: B1/B3 transient 12.5s, 2.3 MB; BBR-SAT 2.5s, 18.6 MB, 44× less queue; CUBIC 10.5s.

### §V-A.3 Lead-Time Sensitivity (current)

Under 1×BDP cap: BBR-SAT T90=4.5s at ℓ=0; drops to 1.5s at ℓ=2s (PREDICTED drain completes just before handover); returns to 4.5s at ℓ≥5s (drain fires too early, BBR re-inflates cwnd). Sweet spot: ℓ ∈ [0, 2] s. B1/B3/B4 are N/C at all lead times.

### §V-A.4 Bandwidth-Increasing and Moderate Transitions (current)

Most baselines converge at T90 ≤ 2.5s (Table IV). B4 catastrophically fails for GEO→LEO (45 MB queue, 0.33 MB goodput). LEO→MEO: all BBR variants N/C (MaxBwFilter plateau at 71% util), CUBIC 5.5s.

---

## Questions for Claude/Gemini Review

1. **Overall narrative:** Is "1×BDP droptail as primary, uncapped as reference" framing now compelling and internally consistent? Any remaining inconsistencies between tables, text, and figures?

2. **MEO→GEO goodput regression:** BBR-SAT gets 17.95 MB vs B1's 20.82 MB (14% worse) despite same T90=0.5s. This is because the full CONFIRMED context switch fires for MEO→GEO (GEO target RTT > 250ms). Should the paper explicitly acknowledge this? Should the CONFIRMED handler be suppressed for MEO→GEO (all T_HO converge in 0.5s anyway)?

3. **LEO→MEO framing:** Is "genuine BBRv3 MaxBwFilter limitation" sufficient, or does the paper need a stronger fix or a stronger disclaimer? CUBIC's 5.5s convergence (via loss) is the only working solution.

4. **Lead-time figure:** The figure (`fig_t90_lead.pdf`) shows uncapped data. The caption notes this. Should we regenerate it with capped data (showing the 4.5→1.5→4.5 pattern)? Or is the uncapped figure acceptable as a reference with the capped behavior described in text?

5. **Abstract T90 claim:** Should we say "as fast as 1.5s with a 2-second advance signal" or keep "4.5s" as the zero-lead-time baseline?

6. **B4 GEO→LEO catastrophe:** 45 MB peak queue despite a 1×BDP cap seems impossible (cap should be ~218 KB for GEO and ~125 KB for LEO). This may indicate a measurement artifact or a genuine corner case where the `queue_delay_max` isn't applied correctly at the moment of handover. Does the characterization "retransmit storm from zero-duration pause" correctly explain the 45 MB?

7. **Fairness data:** The fairness experiments use capped buffers (fairness_v2 already has `queue_delay_max = 2 × latency_us`). Are the fairness numbers (J=0.994 for F1-BBRv3, J=0.887 for F3-CUBIC, J≈0.69 for F1-CUBIC) still the primary claim? Or do they need updating given the capped buffer primary model?

8. **Completeness check:** Is there any claim in the paper that depends on uncapped results but isn't accompanied by a capped-buffer equivalent?

---

## Files Changed Since Review #11

- `picoquic/bbr.c`: adaptive CONFIRMED handler (light-touch min_rtt swap for LEO/MEO targets); MaxBwFilter reset for downward BW transitions; corrected orbit table (upload BW 10/10/3 Mbps)
- `picoquictest/bbr_sat_experiment.c`: DL/UL split in orbit struct; `use_bdp_cap` parameter
- `picoquictest/bbr_sat_fairness.c`: corrected link model (upload bottleneck)
- `picoquic_t/bbr_sat_runner.c`: `use_bdp_cap` flag; CUBIC baseline (5) added
- `experiments/run_capped_sweep.py`: 5400-job sweep script
- `results/capped_sweep_v1/exp1_raw.csv`: 5400 deduplicated rows
- `results/capped_sweep_v1/exp1_summary.csv`: per-condition medians and CIs
- `paper/paper.tex`: full restructure (capped primary, all numbers updated)
- `scripts/make_docx.py`: matching docx updates

---

*Prepared by Claude Code (Sonnet 4.6) — 2026-05-18*
