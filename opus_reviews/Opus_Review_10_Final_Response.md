# Opus Review #10 Response — Final Algorithm + Paper Guidance for Code

**Date:** May 2026  
**Input:** Opus_Review_10_Paper_Draft.md (Code), Gemini strategic analysis  
**Verdict:** Algorithm is sound. Paper needs targeted numerical updates + one simulation re-run.

---

## Q1: No-Op Light Touch — Keep As Literal No-Op

Gemini correctly identified the theoretical min_rtt stale-filter problem on upward RTT transitions. When going LEO→MEO (50 ms → 160 ms), the new RTT is higher than current min_rtt, so BBRv3's windowed-minimum filter ignores the 160 ms samples and clings to stale 50 ms for up to 10 seconds. The cwnd is undersized by 3.2× during this window.

**However, your sweep data proves this doesn't matter in practice.** LEO→MEO: both B1 and BBR-SAT show identical goodput (47.6 vs 47.8 MB). The reason: since LEO and MEO share identical upload BW (10 Mbps), the bottleneck is the link rate, not the cwnd. BBRv3's pacing rate — driven by max_bw (correct at 10 Mbps) — controls throughput, not the cwnd. The cwnd undersizing only matters when cwnd is the binding constraint, which it isn't when BW is unchanged.

On downward RTT transitions (MEO→LEO, GEO→LEO, GEO→MEO), the new RTT is lower — min_rtt immediately accepts it. No staleness.

**Decision: Keep the no-op as implemented.** Don't add a min_rtt swap — it risks reintroducing the regression. The data validates the approach.

**For the paper, add one sentence in §V-A.4:**

> "Because LEO and MEO share identical upload capacity (10 Mbps), BBRv3's pacing rate — driven by max_bw, not min_rtt — correctly matches the bottleneck on both orbits. The stale min_rtt temporarily undersizes the congestion window but does not constrain throughput when the pacing rate is the binding limit."

---

## Q2: MEO→GEO Full Switch — Confirmed Correct

MaxBwFilter zero + max_bw seed at 3 Mbps + ProbeBW_DOWN is right. Sweep confirms: MEO→GEO 1502 ms BBR-SAT vs 1501 ms B1. No regression. No changes needed.

---

## Q3: Paper Narrative — Still Compelling

3.3× BW reduction + 12× RTT increase + 3.5× BDP expansion is a severe transport challenge. The headline result changed from "B1 never converges" to "B1 converges in 12.5s — 5× slower than BBR-SAT's 2.5s, with 8× less goodput." This is actually STRONGER because it's not a simulator artifact.

**Abstract framing:**

> "up to a 3.3× reduction in upload capacity alongside a 12× increase in propagation delay, forcing a 3.5× expansion in the path's bandwidth-delay product"

**Buffer sensitivity re-run is REQUIRED.** Old data used 10 Mbps GEO upload. New model has 3 Mbps, so 1×BDP buffer = 218 KB. Run:

```
LEO→GEO, T=30s, lead=5s, 1×BDP droptail (218 KB), N=10
Baselines: B1, BBR-SAT, CUBIC
Metrics: T90, SS utilization, goodput, loss count, peak queue
```

~15 minutes on N100. Non-negotiable.

---

## Q4: Enable and Forget — ACHIEVED

The sweep proves it:

| Transition | BBR-SAT vs B1 | Path |
|---|---|---|
| LEO→GEO | 5× faster (2.5s vs 12.5s), 8× goodput | Full switch |
| MEO→GEO | Same (1.5s) | Full switch |
| LEO→MEO | Same (identical goodput) | No-op |
| MEO→LEO | Same (0.5s) | No-op |
| GEO→LEO | Same (1.5s) | No-op |
| GEO→MEO | Same (2.5s) | No-op |

Never worse. Dramatically better where it matters. Enable and forget.

---

## Q5: Paper Update Task List

### BLOCKING (before submission)

| # | Task | Effort |
|---|---|---|
| 1 | **Run buffer sensitivity** with corrected link model (3 Mbps GEO, 218 KB buffer) for B1 + BBR-SAT + CUBIC | 15 min |
| 2 | **Run B3 + B4** under corrected model for LEO→GEO at T=30s, ℓ=0 — check if they still fail or now converge via loss | 10 min |
| 3 | **Re-run F1 + F3 fairness** under corrected link model — old results used wrong BW values | 30 min |
| 4 | Update Table II: DL/UL split with new BDP values | 5 min |
| 5 | Update Abstract: "3.3× BW reduction, 12× RTT increase, 3.5× BDP expansion" | 5 min |
| 6 | Update §I: replace all "5×" BW references with "3.3×", update BDP math | 10 min |
| 7 | Update §III.B Phase 2: use the adaptive CONFIRMED description from your §4.1 proposed rewrite — it's good | 10 min |
| 8 | Update Tables III and IV with v4 sweep numbers | 10 min |
| 9 | **Rewrite §V-A.2 narrative.** B1 now converges at 12.5s (not N/C). New framing: "B1 converges in 12.5 s — five times slower than BBR-SAT — and delivers only 2.3 MB of goodput compared to BBR-SAT's 18.7 MB, an 8× improvement." This is stronger than "never converges" because it's not a simulator artifact. | 15 min |
| 10 | Rewrite §V-A.4: remove 8.5s LEO→MEO regression text. Replace with: "For all transitions where target RTT ≤ 250 ms, BBR-SAT applies the no-op path and matches vanilla BBRv3 within measurement precision (Table IV)." | 5 min |
| 11 | Add LEO→MEO N/C metric artifact note (use Gemini's suggested language) | 5 min |
| 12 | Update buffer sensitivity paragraph in §V-A.2 with new numbers from task #1 | 10 min |
| 13 | Remove §VI.A "Buffer model" bullet — it's now primary data in §V-A.2 | 2 min |
| 14 | Update Figure 1: B1 is now a visible data point at 12.5s, not N/C at top | 10 min |

### RECOMMENDED (improves paper, not blocking)

| # | Task | Effort |
|---|---|---|
| 15 | Add pacing-rate-vs-cwnd explanation sentence in §V-A.4 (from Q1 above) | 5 min |
| 16 | Clarify "N=10" as "10 runs with sub-ms jitter for phase variance" | 2 min |

---

## CRITICAL: Narrative Change in §V-A.2

**B1 now converges at 12.5 seconds** under the corrected link model. The paper can NO LONGER say "BBRv3 fails entirely" or "never converges." The correct framing:

> "B1 (vanilla BBRv3) converges in 12.5 s — five times slower than BBR-SAT (2.5 s) — and delivers only 2.3 MB of goodput in the post-handover window compared to BBR-SAT's 18.7 MB, an 8× improvement. The prolonged convergence reflects BBRv3's two-cycle MaxBwFilter requiring approximately 8 × RTT_GEO ≈ 4.6 s to clear stale bandwidth estimates, followed by multiple ProbeBW cycles to ramp from the reduced pacing rate to the GEO bottleneck capacity."

**This is actually a better paper.** A 5× convergence speedup and 8× goodput improvement over a real BBRv3 behavior (not a simulator artifact) is more defensible than "infinity vs 2.5s" on an uncapped queue. Gemini cannot attack it as a strawman.

---

## CRITICAL: Check B3 and B4 Under New Model

With realistic buffers (218 KB) and 3 Mbps GEO upload, B3 (cwnd-freeze) and B4 (pause/resume) may now converge via loss — they failed before because the uncapped queue prevented loss signals from reaching BBRv3. If they now converge:

- B3 might converge in ~5-10s (frozen LEO cwnd causes overshoot, loss triggers backoff, then slow convergence)
- B4 might converge in ~3-6s at ℓ=5 (similar to before but with capped buffer)

**This is fine.** BBR-SAT's 2.5s and 8× goodput advantage over B1 is the primary comparison. B3/B4 becoming less terrible doesn't weaken the story — it just means the Table III results need updating.

---

## Fairness Re-Run Necessity

The F1 and F3 experiments used the old link model (50→10 Mbps for LEO→GEO upload). With the corrected model (10→3 Mbps), the BW step is smaller and the dynamics will differ. The J=0.997 for F1-BBRv3 may hold (fairness is driven by equal CCA, not BW magnitude) but the F1-CUBIC and F3 numbers will change because CUBIC's behavior at 3 Mbps differs from 10 Mbps.

**Re-run F1 (BBR-SAT vs BBRv3 and vs CUBIC) and F3 (BBR-SAT vs CUBIC on GEO) with the corrected link model.** Report new Jain's index values. If J values change by more than 0.05 from old values, update Table V.

---

## Execution Order for Code

1. Buffer sensitivity re-run (B1 + BBR-SAT + CUBIC, 218 KB cap) — 15 min
2. B3 + B4 check under new model — 10 min  
3. F1 + F3 fairness re-run under new model — 30 min
4. Update all paper text per task list — 2 hours
5. Rebuild PDF
6. Send to Opus for final pre-submission review

Total: ~3 hours. Then one more Gemini pass and submit.
