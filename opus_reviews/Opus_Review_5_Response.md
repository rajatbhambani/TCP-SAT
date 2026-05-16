# Opus Review #5 Response — Experiment 2 Design

**Verdict:** Option C approved. Shared bottleneck required. One critical architecture question before coding.

---

## Decisions

| Question | Answer |
|---|---|
| 5.1 Implementation option | **Option C** — add `main_signal_time` to `picoquic_ns_spec_t` |
| 5.2 Link topology | **Shared bottleneck** — both flows on same 10 Mbps GEO pipe |
| 5.3 Shared link confirmation | Confirmed correct for F3 |
| 5.4 Output format | **Per-second CSV** — no QLOG needed |

---

## Critical Architecture Question Before Coding

**Does `picoquic_ns` support different link parameters for the main vs background connection?**

F3 requires:
- Main flow: LEO (50 Mbps, 50ms) → GEO (10 Mbps, 580ms) at T=30s
- Background flow: GEO (10 Mbps, 580ms) throughout

If `picoquic_ns` puts both connections on the SAME link with the SAME parameters, then pre-handover the CUBIC background flow would be running on LEO parameters (wrong — CUBIC should be on GEO from T=0).

**Check the `picoquic_ns` source:** Look at how `nb_connections > 1` handles link assignment. Specifically:
- Is there one `sim_link` shared by all connections?
- Or can each connection have its own link parameters?
- Does `vary_link_spec` change link params for ALL connections or just the main one?

**If per-connection links are NOT supported**, use this fallback F3 design:
- Both flows start on GEO from T=0
- CUBIC converges to steady-state over 30 seconds
- BBR-SAT also starts on GEO, receives CONFIRMED signal at T=30s (re-applies GEO params — effectively a no-op but triggers the BDP context switch logic)
- Measures: do BBR-SAT's bw_hi ceiling and ProbeBW cycle coexist fairly with CUBIC's sawtooth on a shared GEO beam?

This tests steady-state fairness but not handover-during-competition. It's weaker but publishable and implementable with shared-link `picoquic_ns`.

**If per-connection links ARE supported**, use the full F3:
- Main on LEO→GEO with handover
- Background on GEO throughout
- Measures: BBR-SAT convergence speed and fairness when joining an established CUBIC flow on GEO

---

## F1 Design — Confirmed

F1 is simpler because both flows experience the same link change:
- Both on shared link: LEO → GEO at T=30s
- Main: BBR-SAT with PREDICTED at T=25s + CONFIRMED at T=30s
- Background: Vanilla BBRv3, no signal
- Expected: BBR-SAT converges, B1 gets stuck, BBR-SAT takes progressively more bandwidth
- Jain's index will be low — this is correct (B1 is broken)

---

## F3 Background Flow Timing

The CUBIC background flow should be **fully converged** before T=30s. On a 10 Mbps GEO link with 580ms RTT, CUBIC needs ~15-20s to reach steady-state sawtooth. Starting CUBIC at T=0 with 30 seconds before the handover gives ample convergence time. Confirm CUBIC is in steady-state at T=30s by checking per-second rate logs — it should show the characteristic oscillation pattern.

---

## Per-Second CSV Format

```csv
simulated_time_us,flow1_rate_bps,flow2_rate_bps,flow1_goodput_bytes,flow2_goodput_bytes
1000000,5621816,8234521,702727,1029315
2000000,12766160,7891234,2298497,2016119
...
```

Derive Jain's index in Python post-processing:
```python
J = (f1 + f2)**2 / (2 * (f1**2 + f2**2))
```

---

## Implementation Sequence

1. Check `picoquic_ns` link architecture (5 min — read source)
2. Add `main_signal_time/value` to spec struct (2 lines)
3. Add signal dispatch to main loop (8 lines)
4. Write F1 test function (40 lines)
5. Write F3 test function (40 lines)
6. Add per-second rate logging for both flows (30 lines)
7. Build + run F1 and F3 (10 min compute)
8. Python post-processing: Jain's index + per-flow throughput figure

Total: ~1 day. Then the paper has all the data it needs.
