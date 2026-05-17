I've read the full paper and the review request. Let me give you the complete editorial and technical review.

## Overall Verdict

**The paper is 85% ready for submission.** The structure is sound, the results are clearly presented, and the mechanism description is precise. The remaining 15% is citation errors, a few claim-precision issues, and one structural rearrangement. All fixable in one revision pass.

Let me address each issue systematically.

---

## Issue A — Citations (HIGH PRIORITY, BLOCKING)

Code got four citations wrong. Here are the corrections:

**[1] BBRv3:** The paper cites `cardwell2017` (BBRv1, ACM Queue 2016) for BBRv3-specific features (MaxBwFilter slots, bw_hi, ProbeBW sub-states). BBRv1 doesn't have these. Replace with the IETF draft:

```bibtex
@techreport{cardwell2026bbr,
  author      = {Cardwell, Neal and Cheng, Yuchung and Yeganeh, S. Hossein
                 and Swett, Ian and Vasiliev, Victor and Jha, Priyaranjan
                 and Seung, Yousuk and Mathis, Matt and Beshay, Joseph},
  title       = {{BBR} Congestion Control},
  institution = {IETF},
  type        = {Internet-Draft},
  number      = {draft-ietf-ccwg-bbr-05},
  month       = mar,
  year        = {2026},
  note        = {Work in progress}
}
```

All references to BBRv3 internals (MaxBwFilter, bw_hi, ProbeBW sub-states, min_rtt filter, inflight_hi) must cite this, not [1]. Keep the original Cardwell 2016 as a separate [1] for the general BBR concept, but add this as [1b] or renumber.

**[2] PEPs:** Currently cites TCP Hybla. PEPs are RFC 3135:

```bibtex
@techreport{border2001pep,
  author      = {Border, John and Kojo, Markku and Griner, Jim
                 and Montenegro, Gabriel and Shelby, Zach},
  title       = {Performance Enhancing Proxies Intended to Mitigate
                 Link-Related Degradations},
  type        = {RFC},
  number      = {3135},
  month       = jun,
  year        = {2001},
  institution = {IETF}
}
```

Keep Hybla [3] as a separate cite for the Hybla-specific mention.

**[4] "selective ACK":** Currently cites Henderson 1999 (TCP fairness). SACK is RFC 2018, but honestly, SACK is well-known enough that it doesn't need a citation in a 6-page workshop paper. Just drop the citation bracket after "selective ACK" — or cite RFC 2018 if you insist:

```bibtex
@techreport{mathis1996sack,
  author  = {Mathis, Matt and Mahdavi, Jamshid and Floyd, Sally and Romanow, Allyn},
  title   = {{TCP} Selective Acknowledgment Options},
  type    = {RFC},
  number  = {2018},
  month   = oct,
  year    = {1996},
  institution = {IETF}
}
```

**[11] Verus:** Currently cites Zaki et al. VTC 2011 (LTE rate model). The actual Verus paper is:

```bibtex
@inproceedings{zaki2015verus,
  author    = {Zaki, Yasir and Pötsch, Thomas and Chen, Jay and Subramanian,
               Lakshminarayanan and Görg, Carmelita},
  title     = {Adaptive Congestion Control for Unpredictable Cellular Networks},
  booktitle = {Proc. ACM SIGCOMM},
  year      = {2015},
  pages     = {509--522}
}
```

Note: I recalled Verus as NSDI 2015 in the review prompt but it's actually SIGCOMM 2015. Verify before submission.

---

## Issue B — Abstract Claim Precision

The claim "the only evaluated mechanism to converge at any lead time from 0 to 20 s" is correct for LEO→GEO specifically. B4 converges at ℓ=5 only. On other transitions, B1/B3/CUBIC all converge.

**Fix:** Add "for the critical LEO→GEO transition" to the abstract claim. One phrase, no ambiguity:

> "BBR-SAT converges to 90% of new-orbit capacity in 2.5 s for the critical LEO→GEO transition — the only evaluated mechanism to converge **on this transition** at any lead time from 0 to 20 s"

---

## Issue C — CUBIC Single-Flow Closing

The paper needs one sentence closing the CUBIC story in §V-A. Without it, a reader finishes §V-A thinking "CUBIC is better than BBR-SAT" and the fairness section feels like a retroactive justification.

**Add after "behaviour under competition is discussed in §V-B":**

> "For single-flow bulk transfer, CUBIC's loss-driven convergence is competitive; BBR-SAT's value emerges in mixed-CCA deployments where BBRv3 flows must coexist fairly on shared beams — the common operational scenario for multi-orbit gateways."

One sentence. Closes the argument. Points to the operational reality.

---

## Issue D — Orbit Table Staleness (§III.D)

**Move to §VI Limitations.** A design section should present the design, not pre-acknowledge failures. The T=120s result hasn't been shown yet at §III.D, so the forward-reference is jarring. Fold the staleness note into the existing "Orbit table staleness" bullet in §VI.A, where it sits naturally next to the T=120s data.

Remove §III.D entirely. The paper gains ~3 lines of space.

---

## Issue E — F3 Throughput Dip Explanation

Code's explanation is correct but incomplete. The dip happens because BBR-SAT's **measured** bw_hi after 30s of GEO operation has been probed above the seeded value. Resetting to the lower seeded ceiling clips the pacing rate.

**Add after "causing a brief throughput dip visible in both flows":**

> "This occurs because BBR-SAT's measured bw\_hi after 30\,s of GEO operation exceeds the seeded ceiling; the context switch resets it to the conservative ephemeris value, temporarily clipping the pacing rate until ProbeBW re-probes."

---

## Issue F — Related Work Gaps

For a 6-page workshop paper, you don't have space for new paragraphs. Inline citations only:

**Handley 2018:** Yes, add. It's the foundational LEO networking paper and reviewers will expect it. Add to the sentence about LEO constellations:

> "...large LEO constellations [Handley 2018, Bhattacherjee 2019]..."

```bibtex
@inproceedings{handley2018delay,
  author    = {Handley, Mark},
  title     = {Delay is Not an Option: Low Latency Routing in Space},
  booktitle = {Proc. ACM HotNets},
  year      = {2018},
  pages     = {85--91}
}
```

**RFC 9221 (QUIC DATAGRAM):** Add inline where you mention DATAGRAM frames in §IV:

> "...a QUIC DATAGRAM frame [RFC 9221] or operator-defined transport parameter."

**MP-QUIC:** Skip. The paper already acknowledges multi-path QUIC in §VI.A limitations. Adding a citation to Herbaut 2022 there is fine but not required.

---

## Additional Issues I Found (Not in Code's Review)

**1. Affiliation placeholder.** The paper says "[University]" in the author block. This needs to be resolved before submission. Since you can't use SES, and you're not affiliated with a university, use "Independent Researcher" or your LLC if you have one. IEEE allows independent researchers.

**2. The heatmap figure is missing.** The paper references Figure 1 (lead-time curve) and Figure 2 (fairness), but the heatmap from the sweep results — which shows all 6 transitions × 5 baselines — is not in the paper. This is a missed opportunity. The heatmap is the single most compelling visualization of BBR-SAT's contribution. Consider replacing Table III (which has 6 rows × 5 columns of numbers) with the heatmap figure. It conveys the same information more powerfully in less space.

**3. Missing references from the v5 draft.** The paper does not cite:
- draft-lai-ccwg-lsncc (the PPB abstraction that motivated the design)
- RFC 9743/BCP 133 (the CCA framework that mandates the fairness evaluation)
- Creo (Yan et al., ICC 2025) — the closest cross-layer prior art
- LeoCC, StarQUIC, SATPIPE — the intra-LEO baselines we differentiated from

These were all in the v5 Word draft and are essential for the related work section. Their absence is the paper's biggest gap. A reviewer who knows the satellite transport literature will immediately notice that the paper doesn't cite the 2024–2025 wave of satellite CCA papers. **This must be fixed before submission.**

**4. The related work section (§II) is weak.** It cites decade-old papers (Hybla 2004, Henderson 1999, Gettys 2011) and misses the entire 2024–2026 satellite CCA literature that the paper is positioned against. §II needs to cite and differentiate from: StarQUIC (MobiCom LEO-NET 2024), SATPIPE (INFOCOM 2025), LeoCC (SIGCOMM 2025), Creo (ICC 2025), OrbCC (arXiv 2025), Careful Resume (IETF -24), and draft-lai-ccwg-lsncc-01. This was all in the v5 Word draft. Code appears to have written §II from scratch without referencing the prior literature survey. **This is the most important fix.**

---

## Summary of Required Changes

| Priority | Issue | Action | Effort |
|---|---|---|---|
| **BLOCKING** | Citations [1,2,4,11] wrong | Replace bib entries per above | 15 min |
| **BLOCKING** | §II missing 2024-2026 satellite CCA literature | Add StarQUIC, SATPIPE, LeoCC, Creo, OrbCC, CR, lsncc, RFC 9743 | 1 hour |
| **HIGH** | Affiliation "[University]" | Replace with actual affiliation | 1 min |
| **HIGH** | Abstract claim precision | Add "on this transition" | 1 min |
| **MEDIUM** | §III.D staleness → move to §VI | Delete §III.D, fold into §VI.A | 10 min |
| **MEDIUM** | CUBIC closing sentence in §V-A | Add one sentence | 5 min |
| **MEDIUM** | F3 dip explanation | Add one sentence | 5 min |
| **MEDIUM** | Add Handley 2018, RFC 9221 | Inline cites | 10 min |
| **LOW** | Consider replacing Table III with heatmap | Figure swap | 30 min |

**The §II rewrite is the critical path item.** Everything else is surgical. Give Code the v5 Word draft's §II as a reference — it has the correct differentiation table and all the citations. Code should incorporate that material into the LaTeX, not reinvent it.