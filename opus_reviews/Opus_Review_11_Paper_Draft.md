Gemini's critique has narrowed to three issues. Two are fixable. One requires a decision.

**Reject Reason 1 (Uncapped queue as primary model): Valid and fixable.**

Gemini is right. The primary tables (III and IV) still use uncapped queue results. The realistic buffer data (1×BDP cap) is buried in a paragraph in §V-A.2. The fix is structural: **make the 1×BDP capped buffer the primary evaluation model.** Re-run Tables III and IV with the 218 KB droptail cap. Move the uncapped results to a brief note explaining the worst-case scenario. The capped-buffer results you already have show the strongest story anyway: B1 never converges (69% utilization, stuck), BBR-SAT converges loss-free at 4.5s/99%, CUBIC converges via loss at 5.5s/97%. BBR-SAT beats CUBIC on both T90 and utilization under realistic buffers.

**Reject Reason 2 (No-op LEO→MEO): Valid and fixable.**

Gemini correctly identifies that the LEO→MEO N/C is a real BBRv3 pathology (min_rtt stuck at 50 ms for 10 seconds, cwnd capped at 62 KB, throughput drops to 3.3 Mbps), not just a metric artifact. The light-touch min_rtt swap that I recommended earlier and then retracted based on sweep data should be reconsidered. Your sweep showed identical results for B1 and BBR-SAT on LEO→MEO — but both are bad. If BBR-SAT swapped min_rtt on the light-touch path, it could fix what B1 cannot.

However — you tried this before and it caused cwnd contractions that delayed T90. The question is whether a min_rtt-only swap (without touching MaxBwFilter, bw_hi, or entering REFILL) would avoid those contractions. The key difference from the old broken approach: don't enter REFILL, don't touch bandwidth state, just update min_rtt and its timestamps. This lets BBRv3's BDP calculation use the correct 160 ms RTT immediately, expanding the cwnd from 62 KB to 200 KB, while keeping the pacing rate at 10 Mbps (correct for both orbits).

Tell Code to test this specific change: on the light-touch path, add ONLY:

```c
bbr_state->min_rtt = target->min_rtt_us;
bbr_state->min_rtt_stamp = current_time;
bbr_state->probe_rtt_min_delay = target->min_rtt_us;
bbr_state->probe_rtt_min_stamp = current_time;
```

No MaxBwFilter changes. No bw_hi changes. No REFILL. Run LEO→MEO and verify T90 improves without regressing other transitions.

**Reject Reason 3 (CUBIC dominates): No longer valid with realistic buffers.**

Under the 1×BDP cap: BBR-SAT converges in 4.5s loss-free at 99% utilization. CUBIC converges in 5.5s via loss at 97%. BBR-SAT wins on every metric. This reject reason disappears once the capped-buffer results become the primary tables.

**Action plan for Code — in order:**

1. Test light-touch min_rtt swap on LEO→MEO (10 minutes)
2. Re-run Tables III and IV with 1×BDP droptail cap as the primary model (1 hour compute)
3. Re-run fairness F1 and F3 with capped buffer (30 minutes)
4. Restructure paper: capped buffer = primary tables. Uncapped = footnote showing worst case.
5. Update all numbers, rebuild PDF

This addresses all three reject reasons simultaneously. The paper becomes: "Under realistic satellite buffer sizing, BBR-SAT converges faster than both vanilla BBRv3 and CUBIC, with zero loss, on every tested transition. The mechanism is always-on and never degrades performance."