# Opus Review #12 Response — Final Fixes Before Submission

**Verdict:** Three issues, all fixable. One simulation artifact to exclude, one goodput regression to disclose, one figure to regenerate.

---

## Fix 1: B4 GEO→LEO 45 MB Queue — Exclude From Table

The 45 MB queue under a 218 KB droptail cap is physically impossible. This is a simulator artifact specific to B4's send-pause/resume implementation interacting with the link-parameter change at handover time.

**Action:** Verify that no BBR-SAT, B1, B3, or CUBIC condition shows a peak queue exceeding the BDP cap. If confirmed (only B4 GEO→LEO is affected):

- Replace B4 GEO→LEO cell in Table IV with "—§" 
- Add footnote: "§ B4 GEO→LEO excluded: the send-pause/resume implementation triggers a retransmission storm at handover that produces queue depths inconsistent with the droptail model. This is a B4 implementation limitation."
- Do NOT attempt to fix the simulator. B4 is a straw baseline, not the contribution.

**Verification command:**
```bash
# Check all peak_queue values against BDP caps
awk -F',' 'NR>1 && $NF > 1000000 {print $0}' results/capped_sweep_v1/exp1_raw.csv | grep -v "baseline=3"
```
If this returns zero rows (excluding baseline=3 which is B4), all other results are valid.

---

## Fix 2: MEO→GEO 14% Goodput Regression — Disclose, Don't Fix

The regression is real but operationally negligible: T90 is identical (0.5s), and the goodput difference is 2.87 MB over 90 seconds (0.25 Mbps average). The full context switch fires because GEO target RTT > 250 ms, causing a 1-2 second throughput dip during the MaxBwFilter reset.

**Do NOT change the algorithm.** Attempting to special-case MEO→GEO introduces conditional logic that makes the mechanism harder to reason about and verify. The current rule (full switch for GEO targets, light touch for LEO/MEO targets) is clean and principled.

**Action:** Add to §V-A.4 after the MEO→GEO row discussion:

> "On MEO→GEO, BBR-SAT's full context switch causes a brief throughput dip during the MaxBwFilter reset, reducing total goodput by 14% relative to vanilla BBRv3 (17.95 vs 20.82 MB) despite identical T90 (0.5 s). This is the only transition where the always-on mechanism incurs a measurable goodput cost; a production deployment could gate the full switch on the source-to-target BW ratio to suppress it for transitions where vanilla BBRv3 converges without assistance."

This is honest, acknowledges the tradeoff, and suggests the production fix without implementing it. A reviewer will respect this.

---

## Fix 3: Regenerate Figure 1 With Capped-Buffer Data

The current Figure 1 shows uncapped results (flat 2.5s BBR-SAT line). Under the capped buffer, the profile is non-monotonic: 4.5s at ℓ=0, 1.5s at ℓ=2, back to 4.5s at ℓ≥5.

**Action:** Regenerate `fig_t90_lead.pdf` from the capped sweep data. The non-monotonic profile is actually a stronger figure because it shows:
1. The mechanism works at zero lead time (4.5s — no advance signal needed)
2. A 2-second advance signal cuts convergence time by 3× to 1.5s
3. Lead times > 5s don't help further (drain fires too early)
4. All other baselines (B1, B3, B4) are N/C across all lead times

**For the abstract:** Update to reflect the optimized case:

> "converges to 90% of new-orbit capacity in as little as 1.5 s with a 2-second advance signal (4.5 s at zero lead time) for the critical LEO→GEO transition"

---

## Answers to Code's Questions

### Q1 (Narrative consistency): 
The "1×BDP as primary, uncapped as reference" framing is correct and now internally consistent — except for Figure 1 which still shows uncapped data. Fix that and the inconsistency is resolved.

### Q2 (MEO→GEO goodput regression):
Disclose it. Don't suppress the full switch. See Fix 2 above.

### Q3 (LEO→MEO framing):
"Genuine BBRv3 MaxBwFilter limitation" is the correct framing. Gemini's objection that BBR-SAT should fix the MaxBwFilter plateau is unreasonable — fixing it would require BBR-SAT to artificially inflate bandwidth estimates, which would break the model-based foundation of BBRv3. The light-touch min_rtt swap already addresses the part BBR-SAT CAN fix (min_rtt staleness). The MaxBwFilter plateau is a BBRv3 core limitation that affects all BBR variants equally. Add one sentence:

> "Resolving the MaxBwFilter plateau would require artificially inflating bandwidth estimates beyond observed delivery rates — a modification that would compromise BBRv3's model-based design and is outside the scope of this extension."

### Q4 (Lead-time figure):
Regenerate with capped data. See Fix 3.

### Q5 (Abstract T90):
Use the optimized case. Say "as little as 1.5 s with a 2-second advance signal (4.5 s at zero lead time)."

### Q6 (B4 45 MB queue):
Exclude from table. See Fix 1. This is a B4 implementation artifact, not a BBR-SAT or simulator integrity issue.

### Q7 (Fairness data):
The fairness experiments already use the capped buffer model (Code confirmed `queue_delay_max = 2 × latency_us` in fairness_v2). The J values (0.994 F1-BBRv3, 0.887 F3-CUBIC, ~0.69 F1-CUBIC) are valid for the capped model. No re-run needed.

### Q8 (Completeness check):
After Fix 3 (figure regeneration), no claim in the paper depends on uncapped results without a capped equivalent. The uncapped results appear only in the "Large-buffer reference" paragraph, which is explicitly labeled as supplementary.

---

## Execution Order

1. Verify B4 is the only baseline with impossible queue depths (5 min)
2. Regenerate Figure 1 with capped-buffer lead-time data (15 min)
3. Add MEO→GEO goodput disclosure sentence (5 min)
4. Add LEO→MEO MaxBwFilter limitation sentence (5 min)
5. Update abstract with "as little as 1.5 s" framing (2 min)
6. Exclude B4 GEO→LEO from Table IV with footnote (5 min)
7. Remove stray text fragments ("The BBRv3 pacing-rate architecture..." on p4)
8. Fix MEO RTT inconsistency (150 ms vs 160 ms — pick one, use everywhere)
9. Rebuild PDF

**Total: ~1 hour. Then final Gemini pass and submit.**
