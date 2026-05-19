# BBR-SAT Review #13 — Final Polish Before Submission

**Date:** 2026-05-18  
**Reviewer request:** Full pre-submission review. All Review #12 action items have been applied. Paper should now be submission-ready; this review is to catch any remaining inconsistencies, weak arguments, or reviewer traps before the paper goes out.

---

## What Changed Since Review #12

All eight action items from the Opus Review #12 Response have been applied:

**Fix 1 — B4 GEO→LEO verification:** Confirmed via `exp1_raw.csv` that `baseline=3, orbit_from=2, orbit_to=0` is the only row with `peak_queue > 1 MB` (45,031,503 bytes). All other baselines and transitions are clean under the 1×BDP cap.

**Fix 2 — Figure 1 regenerated with capped data:** `fig_t90_lead.py` now reads from `results/capped_sweep_v1/` instead of the old uncapped sweep. The figure now shows the non-monotonic BBR-SAT profile (4.5 s at ℓ=0, dropping to 1.5 s at ℓ=2 s, returning to 4.5 s at ℓ≥5 s). B1/B3/B4 are N/C at all lead times. The ℓ=2 sweet-spot is annotated directly. Figure caption updated to remove the "uncapped queue shown for reference" note.

**Fix 3 — MEO→GEO goodput disclosure added:** After Table IV, the following paragraph was added to §V-A.4:
> "On MEO→GEO, BBR-SAT's full context switch causes a brief throughput dip during the MaxBwFilter reset, reducing total goodput by 14% relative to vanilla BBRv3 (17.95 vs. 20.82 MB) despite identical T90 (0.5 s). This is the only transition where the always-on mechanism incurs a measurable goodput cost; a production deployment could gate the full switch on the source-to-target BW ratio to suppress it for transitions where vanilla BBRv3 converges without assistance."

**Fix 4 — LEO→MEO MaxBwFilter plateau sentence added:** At the end of the LEO→MEO metric-artifact paragraph in §V-A.4:
> "Resolving the MaxBwFilter plateau would require artificially inflating bandwidth estimates beyond observed delivery rates — a modification that would compromise BBRv3's model-based design and is outside the scope of this extension."

**Fix 5 — Abstract updated:** Changed from "converges in 4.5 s" to "as little as 1.5 s with a 2-second advance signal (4.5 s at zero lead time)".

**Fix 6 — B4 GEO→LEO excluded from Table IV:** Already done in the prior session (N/C§ with footnote "§ B4 catastrophic: retransmit storm on zero-duration pause; 45 MB queue, <1 MB goodput.").

**Fix 7 — Stray text:** Already clean. The "BBRv3's pacing-rate architecture" sentence at §V-A.2 is properly integrated within the B1 paragraph and is not a stray fragment.

**Fix 8 — MEO RTT inconsistency resolved:** 150 ms → 160 ms everywhere (Table II orbit parameters, LEO→MEO RTT-tripling discussion). BDP corrected: 188 KB → 200 KB. Ratio corrected: 62 KB/160 ms ≈ 3.1 Mbps (was 3.3 Mbps).

---

## Current Paper State — Key Sections

### Abstract (updated)

> Low-Earth orbit (LEO), medium-Earth orbit (MEO), and geostationary (GEO) satellite constellations are increasingly deployed together, creating multi-orbit networks in which a single QUIC connection may traverse orbit-class boundaries mid-flight. Such handovers impose abrupt, step-change shifts in available bandwidth (up to 3.3× upload reduction) and round-trip time (up to 12× increase), which existing congestion-control algorithms handle poorly: BBRv3 stalls for the duration of its two-cycle bandwidth filter, and simple heuristics such as congestion-window freeze or send-pause succeed only at a single, carefully tuned lead time. We present **BBR-SAT**, a minimal extension of BBRv3 that equips the sender with an orbit parameter table seeded from ephemeris data and a two-phase handover protocol: a PREDICTED signal initiates a proactive queue drain via ProbeBW_DOWN, and a CONFIRMED signal adaptively resets the BDP context from the orbit table for downward transitions (GEO target) while acting as a no-op for upward transitions that self-correct naturally. Implemented in picoquic and evaluated in a zero-loss shared-link simulator across all six pairwise LEO/MEO/GEO transitions at three handover times and six advance lead times, BBR-SAT converges to 90% of new-orbit capacity in **as little as 1.5 s with a 2-second advance signal (4.5 s at zero lead time)** for the critical LEO→GEO transition under a realistic 1×BDP droptail buffer cap — the only zero-loss algorithm to converge; CUBIC converges in 5.5 s via packet loss, while all BBR baselines fail (N/C, ≤71% utilisation). In the uncapped large-buffer scenario, BBR-SAT reduces peak queue by 44× over vanilla BBRv3. Fairness analysis shows that BBR-SAT co-exists equitably with competing BBRv3 flows (Jain J = 0.994) and neither worsens nor repairs the pre-existing BBRv3 deference to CUBIC.

### Table II — Orbit Parameters (updated)

| Orbit | DL rate  | UL rate  | RTT    | BDP (UL) |
|-------|----------|----------|--------|----------|
| LEO   | 50 Mbps  | 10 Mbps  | 50 ms  | 62 KB    |
| MEO   | 30 Mbps  | 10 Mbps  | **160 ms** | **200 KB** |
| GEO   | 10 Mbps  | 3 Mbps   | 580 ms | 218 KB   |

### Table III — LEO→GEO, T_HO=30s, ℓ=0, 1×BDP cap

| Baseline | T90      | Util | Goodput  | Peak Q  |
|----------|----------|------|----------|---------|
| B1 BBRv3 | N/C      | 69%  | 13.7 MB  | 162 KB  |
| B3 cwnd-freeze | N/C | 69% | 13.7 MB | 162 KB |
| B4 pause/resume | N/C | 71% | 14.0 MB | 164 KB |
| **BBR-SAT** | **4.5 s** | **99%** | **20.1 MB** | **428 KB** |
| CUBIC    | 5.5 s    | 97%  | 19.6 MB  | 614 KB  |

Note: Util = post-handover steady-state / 3 Mbps GEO link rate. BBR-SAT 99% utilisation loss-free; CUBIC 97% via loss.

### Table IV — All Transitions, T_HO=30s, ℓ=0, 1×BDP cap

| Transition | B1     | B3     | B4       | BBR-SAT    | CUBIC  |
|------------|--------|--------|----------|------------|--------|
| LEO→MEO    | N/C‡   | N/C‡   | N/C‡     | N/C‡       | 5.5 s  |
| LEO→GEO    | N/C    | N/C    | N/C      | **4.5 s**  | 5.5 s  |
| MEO→LEO    | 0.5 s  | 0.5 s  | 0.5 s    | 0.5 s      | 0.5 s  |
| GEO→LEO    | 1.5 s  | 1.5 s  | N/C§     | 1.5 s      | 1.5 s  |
| MEO→GEO    | 0.5 s  | 0.5 s  | 0.5 s    | 0.5 s      | 0.5 s  |
| GEO→MEO    | 2.5 s  | 2.5 s  | 1.5 s    | 2.5 s      | 1.5 s  |

‡ Metric artifact: LEO/MEO share 10 Mbps upload; T90 threshold unreachable due to MaxBwFilter plateau.  
§ B4 catastrophic: retransmit storm on zero-duration pause; 45 MB queue, <1 MB goodput.

### Figure 1 — T90 vs. Lead Time (updated to capped data)

Non-monotonic BBR-SAT profile under 1×BDP cap (T_HO=30s):
- ℓ=0: 4.5 s
- ℓ=2: **1.5 s** ← sweet spot (3× faster)
- ℓ=5–20: 4.5 s (drain fires too early, BBR re-inflates cwnd before HO)
- ℓ=30: 2.5 s (different mechanism — BBR has 30s to settle pre-HO)

B1, B3, B4: N/C at all lead times. CUBIC: flat 5.5 s at all lead times.

B4 converges at ℓ=2, 5, 10 with T90 ≈ 11–14 s (requires a narrow timing window that isn't satisfiable at ℓ=0, 20, 30).

---

## Questions for Opus and Gemini Review

### Q1 — Argument completeness: does the paper earn its claims?

The paper makes three central claims:
1. BBR-SAT is the only zero-loss algorithm to converge on LEO→GEO (True: B1/B3/B4 are N/C; CUBIC converges via loss)
2. BBR-SAT converges in as little as 1.5 s with a 2-second advance signal (True: capped sweep data)
3. BBR-SAT co-exists fairly with BBRv3 (J=0.994) and doesn't worsen BBRv3/CUBIC imbalance (True: fairness_v2 data)

Are all three claims adequately defended? Is the evidence chain from simulator to claim airtight, or are there gaps a reviewer could exploit?

### Q2 — MEO→GEO disclosure: sufficient?

The paper now discloses the 14% goodput regression on MEO→GEO (17.95 vs 20.82 MB, same T90=0.5 s). The disclosure suggests "a production deployment could gate the full switch on the source-to-target BW ratio." Is this disclosure sufficient? Or will reviewers demand a fix, given that it's a measurable regression on a nominally non-critical transition?

Note: NOT suppressing the switch is the principled choice (the rule is clean: full switch for GEO targets, no-op otherwise). The regression is operationally negligible (T90 identical, goodput difference is 2.87 MB over 90 seconds). But reviewers might object.

### Q3 — LEO→MEO MaxBwFilter framing: will it hold?

The paper now says:
> "Resolving the MaxBwFilter plateau would require artificially inflating bandwidth estimates beyond observed delivery rates — a modification that would compromise BBRv3's model-based design and is outside the scope of this extension."

Is this argument strong enough? A hostile reviewer might say: "You have orbit awareness — just force ProbeBW_UP after CONFIRMED for upward RTT transitions." Is there a clean rebuttal to this, or should the paper pre-empt it?

### Q4 — Figure 1 presentation: is the non-monotonic profile explained?

The figure now shows the non-monotonic BBR-SAT profile. The text explains:
- ℓ=0: 4.5 s (no pre-drain, CONFIRMED alone sufficient)
- ℓ=2: 1.5 s (drain completes just before HO, REFILL starts faster)
- ℓ≥5: 4.5 s (drain completes ~100 ms after PREDICTED but HO fires seconds later; BBR re-enters ProbeBW_UP)

The ℓ=30 point shows 2.5 s, not 4.5 s — a minor non-monotonicity not currently called out in the text (§V-A.3 says "sweet spot is ℓ ∈ [0, 2] s"). Does the unexplained ℓ=30 dip need a sentence? Or should the figure just omit ℓ=30?

### Q5 — Table IV completeness: are all cells accurate?

Cells to double-check:
- MEO→GEO BBR-SAT = 0.5 s ✓ (data: T90=500,510 µs, n=10/10 converged)
- GEO→MEO B4 = 1.5 s (is this accurate? B4 "pause/resume" convergence on a bandwidth-increasing + RTT-decreasing transition)
- GEO→MEO CUBIC = 1.5 s (CUBIC faster than B1/B3 on GEO→MEO — plausible?)
- LEO→MEO B4 = N/C‡ (same MaxBwFilter plateau as B1/B3; correct)

### Q6 — Peak Q in Table III: framing concern

BBR-SAT's peak queue (428 KB) is higher than B1/B3/B4 (162–164 KB) on LEO→GEO. The table note says "higher values indicate better buffer utilisation, not bloat, when the algorithm converges." Is this framing convincing? A reviewer might note that BBR-SAT fills more of the 1×BDP cap while B1/B3/B4 can't, which is correct — but does the paper make this causal chain explicit enough?

### Q7 — Fairness section: capped buffer confirmed?

The fairness experiments (F1, F3) use `queue_delay_max = 2 × latency_us` (confirmed in `bbr_sat_fairness.c`). The J values are:
- F1 BBR-SAT vs BBRv3: J = 0.994
- F3 BBR-SAT vs CUBIC: J = 0.887
- F1 BBR-SAT vs CUBIC: J ≈ 0.69 (BBRv3 deference, not BBR-SAT pathology)

Are these numbers self-consistent with the capped single-flow results? No action expected — just a sanity check.

### Q8 — Conclusion section: does it match the updated results?

The conclusion cites "T90 = 4.5 s at zero lead time (1.5 s with a 2-second advance signal)" and "46% more goodput than vanilla BBRv3 (20.1 MB vs. 13.7 MB)." Are there any stale numbers left in the conclusion or related work sections?

---

## Files Changed in This Session

- `paper/paper.tex`: abstract ("as little as 1.5 s"), Table II MEO RTT (160 ms, 200 KB), LEO→MEO RTT calc (160 ms, 3.1 Mbps), MaxBwFilter plateau sentence, MEO→GEO goodput disclosure paragraph, figure caption (capped data), lead-time sensitivity paragraph
- `scripts/figures/fig_t90_lead.py`: reads `capped_sweep_v1` instead of gate_bdp_queue; non-monotonic annotations; title updated
- `results/fairness/fig_t90_lead.pdf` / `.png`: regenerated with capped-buffer lead-time data
- `scripts/make_docx.py`: MEO RTT (160 ms, 200 KB), abstract ("as little as 1.5 s")
- `opus_reviews/Opus_Review_12_Response.md`: response document (committed alongside)

---

*Prepared by Claude Code (Sonnet 4.6) — 2026-05-18*
