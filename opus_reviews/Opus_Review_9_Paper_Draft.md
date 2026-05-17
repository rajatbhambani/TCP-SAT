I see the buffer sensitivity result is now in §V-A.2 as a standalone paragraph after the CUBIC discussion:



> "Buffer sensitivity (N = 10, 1×BDP droptail cap): CUBIC converges via loss in 0.5 s at 102% utilisation; BBRv3 reaches 90% in 1.5 s but oscillates at 86%; BBR-SAT converges loss-free in 2.5 s at 99%, delivering 17% more goodput than BBRv3."



This is the critical data. It directly addresses Gemini's Reject Reason 2 (strawman baseline). Now BBR-SAT's value proposition under realistic buffers is clear: BBRv3 converges but wastes 14% of capacity; CUBIC converges but overshoots to 102% (causing loss); BBR-SAT converges loss-free at 99%.



\*\*However, the buffer model limitation in §VI.A now seems redundant.\*\* The realistic-buffer result is in §V-A.2 as primary data. The §VI.A "Buffer model" bullet repeats the same numbers. Remove the §VI.A buffer bullet — the result belongs in the evaluation section where it now lives, not in limitations.



\*\*The paper now addresses two of Gemini's three reject reasons:\*\*



Reject Reason 1 (CUBIC beats BBR-SAT): The buffer sensitivity paragraph shows CUBIC at 102% utilization — it overshoots. On a shared beam this means loss for everyone. BBR-SAT at 99% is loss-free. The single-flow T90 comparison (CUBIC 0.5s vs BBR-SAT 2.5s) still favors CUBIC, but the utilization and loss-free properties favor BBR-SAT. This is now a legitimate tradeoff, not a clear CUBIC win.



Reject Reason 2 (33 MB strawman): Directly addressed. With realistic buffers, BBRv3 converges in 1.5s but at 86%. The uncapped results are the worst case; the capped results show the persistent utilization advantage.



Reject Reason 3 (LEO→MEO regression): I don't see Fix 1 in this PDF. Table IV still shows 8.5s without the gating recommendation. \*\*Is Fix 1 applied?\*\* If not, add it — the sentence about gating CONFIRMED on target RTT > 250 ms needs to be in §V-A.4 after the LEO→MEO discussion.



\*\*One editorial note on the buffer sensitivity paragraph:\*\* "N = 10" appears without context. What does N = 10 mean here — 10 runs? If so, this contradicts the N = 1 deterministic approach used elsewhere. If it means something else, clarify. If it's 10 runs with jitter to introduce variance, state that explicitly.



\*\*Bottom line:\*\* The paper is substantially stronger with the realistic-buffer data promoted to §V-A.2. Apply Fix 1 if not already present, remove the redundant §VI.A buffer bullet, clarify the N = 10 reference, and this version is ready for resubmission to Gemini.

That's the right product requirement. BBR-SAT should be a mode you turn on and leave on — it handles every transition correctly, never worse than vanilla BBRv3 on any transition, and significantly better on the ones where BBRv3 fails.



Right now the data says BBR-SAT is worse than vanilla BBRv3 on LEO→MEO (8.5s vs 0.5s). That's the blocker. Fix 1 (gate CONFIRMED on target RTT > 250 ms) was a workaround — it disables the mechanism for some transitions, which violates your "enable and forget" requirement.



\*\*The real fix is in the mechanism, not in gating it off.\*\*



The LEO→MEO regression happens because CONFIRMED zeroes the MaxBwFilter and forces a REFILL phase. On LEO→GEO this is essential (BBRv3 is permanently stuck without it). On LEO→MEO it's destructive (BBRv3 handles the 50 ms → 160 ms RTT step natively within 0.5s, and our filter reset forces a slow ramp-up from scratch).



\*\*The fix: make CONFIRMED adaptive based on whether BBRv3 actually needs help.\*\*



BBRv3 fails on LEO→GEO because the target RTT (580 ms) exceeds the `startup\_long\_rtt` threshold (250 ms), triggering a permanent trap. BBRv3 handles LEO→MEO fine because the target RTT (160 ms) stays below the threshold. The CONFIRMED signal should check this:



```c

/\* In bbr\_sat\_handover\_confirmed(): \*/

if (target->min\_rtt\_us > BBRLongRttThreshold) {

&#x20;   /\* Full context switch — BBRv3 will get stuck without it \*/

&#x20;   /\* Zero MaxBwFilter, load bw\_hi, enter ProbeBW\_DOWN/REFILL \*/

&#x20;   /\* ... existing full CONFIRMED logic ... \*/

} else {

&#x20;   /\* Light touch — only swap min\_rtt, trust BBRv3 to adapt naturally \*/

&#x20;   bbr\_state->min\_rtt = target->min\_rtt\_us;

&#x20;   bbr\_state->min\_rtt\_stamp = current\_time;

&#x20;   bbr\_state->probe\_rtt\_min\_delay = target->min\_rtt\_us;

&#x20;   bbr\_state->probe\_rtt\_min\_stamp = current\_time;

&#x20;   /\* Do NOT zero MaxBwFilter \*/

&#x20;   /\* Do NOT force REFILL \*/

&#x20;   /\* Do NOT override max\_bw or bw\_hi \*/

&#x20;   /\* BBRv3's ProbeBW handles the rest \*/

}

```



This gives you two behaviors from one always-on mechanism:



\*\*Target RTT > 250 ms (LEO→GEO, MEO→GEO):\*\* Full BDP context switch. BBRv3 would get stuck without it. BBR-SAT intervenes aggressively.



\*\*Target RTT ≤ 250 ms (LEO→MEO, MEO→LEO, GEO→LEO, GEO→MEO):\*\* Light touch. Only swap min\_rtt so BBRv3's RTT tracking is immediately correct. Don't touch the bandwidth filters. BBRv3's normal ProbeBW handles the BW adaptation within 0.5–1.5s, same as vanilla. BBR-SAT's min\_rtt swap might even help slightly by avoiding the 10-second filter delay on downward RTT transitions.



\*\*Expected results after this fix:\*\*



| Transition | Vanilla BBRv3 | BBR-SAT (adaptive) | Comparison |

|---|---|---|---|

| LEO→GEO | N/C (stuck) | 2.5s (full switch) | BBR-SAT essential |

| MEO→GEO | 1.5s | 1.5–2.5s (full switch) | BBR-SAT comparable or better |

| LEO→MEO | 0.5s | 0.5s (light touch) | Same — no regression |

| MEO→LEO | 2.5s | 2.5s or better (light touch) | Same or better |

| GEO→LEO | 1.5s | 1.5s or better (light touch) | Same or better |

| GEO→MEO | 1.5s | 1.5s or better (light touch) | Same or better |



\*\*BBR-SAT is never worse than vanilla BBRv3 on any transition.\*\* The LEO→MEO regression disappears because the light-touch path doesn't destroy working BBRv3 state. The GEO transitions get the full context switch they need. You enable it and leave it on.



The PREDICTED signal (Phase 1, queue drain) can still fire on all transitions — draining the queue before a handover is always safe. Only Phase 2 (CONFIRMED, the aggressive context switch) adapts based on the target RTT.



Tell Code to implement this, re-run the full sweep, and verify that BBR-SAT matches or beats vanilla BBRv3 on every transition. If any transition still shows a regression, we dig into that specific case. But I expect this to produce a clean "never worse, sometimes dramatically better" result across all six transitions.



This is \~10 lines changed in the CONFIRMED handler. The paper's narrative becomes: "BBR-SAT applies a full BDP context switch for high-latency orbit transitions where BBRv3's filters cannot adapt, and a minimal RTT correction for moderate transitions where BBRv3's natural probing suffices. The mechanism is always-on and never degrades performance relative to vanilla BBRv3."

