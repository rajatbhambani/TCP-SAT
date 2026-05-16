# Opus Review #7 — Full Paper Draft Review Request

**Date:** 2026-05-16  
**Status:** Paper draft complete (6 pages, GLOBECOM Workshop format). Preliminary self-review done. Requesting Opus-level review of technical accuracy, argumentation, and remaining open issues.  
**Paper location:** `paper/paper.tex` (source), `paper/paper.pdf` (compiled)  
**Figures:** `results/fairness/fig_t90_lead.pdf`, `results/fairness/fig_fairness.pdf`  
**Data:** `results/exp_full/exp1_raw.csv` (single-flow), `results/fairness/f1_sweep_run{1,2}.csv`, `results/fairness/f1_cubic_sweep_run{1,2}.csv`, `results/fairness/f3_run{1,2}.csv`

---

## Project Context

BBR-SAT is a minimal extension of BBRv3 for satellite inter-orbit handovers (LEO↔MEO↔GEO). The core idea: satellite operators know handover times from orbital mechanics. BBR-SAT consumes two signals:
- **PREDICTED**: fires at T−ℓ seconds, initiates proactive queue drain via ProbeBW_DOWN
- **CONFIRMED**: fires at T_HO, resets {bw_hi, min_rtt, inflight_hi, MaxBwFilter} from a pre-seeded orbit table

Implemented in picoquic (C). Evaluated in the embedded `picoquic_ns` simulator. Zero-loss throughout.

Key results:
- LEO→GEO T90 = 2.5 s (only baseline to converge at ℓ=0–20 s; B1/B3/B4 all N/C)
- Peak queue: 910 KB vs 33 MB for vanilla BBRv3 (36× reduction)
- Fairness: J=0.997 vs BBRv3, J=0.888 vs CUBIC (steady GEO), J≈0.71 vs CUBIC during handover (inherited from vanilla BBRv3)

---

## Paper Structure (6 pages)

```
§I    Introduction          (~0.6 col)
§II   Background/Related    (~0.8 col) — BBRv3, satellite TCP/QUIC, CCA for handovers
§III  BBR-SAT Design        (~1.0 col) — orbit table, signal protocol, two-phase handover
§IV   Methodology           (~0.6 col) — picoquic_ns, orbit params, baselines, exp parameters
§V    Evaluation            (~2.5 col) — §V-A single-flow (4 sub-subs), §V-B fairness (4 sub-subs)
§VI   Discussion            (~0.4 col) — zero-lead sufficiency + ceiling; limitations
§VII  Conclusion            (~0.3 col)
```

---

## Issues Fixed in This Version (since initial draft)

The following issues from the preliminary self-review have been addressed:

1. ✅ "BDP change of 14×" removed — replaced with "5× rate reduction, ~12× RTT increase"
2. ✅ MaxBwFilter description corrected — bw_hi vs MaxBwFilter distinction now accurate
3. ✅ Probe-cycle RTT parenthetical fixed — "each spanning ~4 RTT_new" added
4. ✅ Buffer table: renamed column from "Buffer (2×RTT)" to "BDP"; added note explaining uncapped queue model
5. ✅ "GEO RTTs" → "LEO RTTs" (pre-handover drain uses current orbit RTT)
6. ✅ LEO→MEO T90 anomaly explained (8.5 s vs 0.5 s for B1/B3 — REFILL at MEO RTT overhead)
7. ✅ B2 baseline gap noted (parenthetical in §IV baselines)
8. ✅ F2 scenario gap explained (reduces to F3, noted inline)
9. ✅ Signal delivery mechanism noted (alg_notify prototype; QUIC DATAGRAM in production)
10. ✅ ℓ=60 removed from parameter list (not reported in results)
11. ✅ "<30 lines" → "<100 lines" (more accurate)
12. ✅ Single-flow vs two-flow distinction clarified in §IV
13. ✅ Discussion §VI condensed (two subsections merged into one paragraph)
14. ✅ "the full GEO buffer" phrase removed (queue is uncapped)

---

## Open Issues Requiring Opus Judgment

### Issue A — Citation Accuracy (HIGH PRIORITY)

The following citations are incorrect and need replacement:

| Location | Current cite | Problem | Correct reference |
|---|---|---|---|
| §II "PEPs" | `caini2002` (TCP Hybla) | PEPs ≠ Hybla | RFC 3135 (Border et al., 2001) or Caini, INTELSAT 2012 |
| §II "selective ACK" | `Henderson2001` (TCP fairness) | Not SACK | RFC 2018 (Mathis et al., 1996) or SACK survey |
| §II "Verus" | `zaki2015` (LTE rate model, VTC 2011) | Wrong paper entirely | Zaki et al., NSDI 2015 "Adaptive Congestion Control for Unpredictable Cellular Networks" |
| All BBRv3 features | `cardwell2017` (BBRv1, ACM Queue 2016) | BBRv3 features not in 2017 paper | Need IETF I-D: draft-cardwell-iccrg-bbr-congestion-control |

**Request:** Suggest correct bib entries for these four cases, or confirm which citations to simply remove if the claim doesn't need a citation (e.g., SACK is well-known enough to cite as RFC directly).

### Issue B — Abstract Claim Precision

The abstract says: *"the only evaluated mechanism to converge at any lead time from 0 to 20 s"*

This is true for the LEO→GEO transition specifically. But B4 (pause/resume) also converges at ℓ=5 s on LEO→GEO. The statement as written implies BBR-SAT is the only mechanism to converge at **any** lead time, which is true overall (B4 only converges at one point). However, on *all other transitions*, B1/B3/CUBIC all converge with T90 ≤ 3.5 s.

**Request:** Does the abstract claim need qualifying ("for the critical LEO→GEO transition") or is it fine as a universal statement given that LEO→GEO is the named focus?

### Issue C — CUBIC Single-Flow Performance

CUBIC achieves better single-flow numbers than BBR-SAT on LEO→GEO (66.4 MB vs 56.9 MB goodput; 1.5 s vs 2.5 s T90; 100% vs 92% utilization). The paper defers entirely to the fairness section ("its behaviour under competition is discussed in §V-B") without ever completing the argument for why a BBR-based deployment would choose BBR-SAT over CUBIC.

The implicit argument is: if you run a mixed BBR fleet, BBR-SAT is needed for fair coexistence. But this is never stated.

**Request:** Should we add one sentence in §V-A closing CUBIC's story, or is the cross-reference sufficient for a 6-page workshop paper?

### Issue D — Orbit Table Staleness Placement

The "Orbit Table Invariants" subsection (§III.D) discusses the T=120 s failure before that result has been shown. It forward-references §V-A. This is unusual — normally design sections don't pre-acknowledge failures.

**Request:** Is it better to:
1. Keep in §III as a known limitation of the design (reviewer sees it as honest up-front disclosure)
2. Move to §VI Limitations (reviewer sees it as an evaluation-derived limitation)
3. Remove from §III and only mention in §VI

### Issue E — Fairness Section F3 Throughput Dip

The F3 section says: "At t=30 s BBR-SAT re-applies its stored GEO BDP context (a no-op on link parameters, but it resets bw_hi, min_rtt, and inflight_hi to the seeded values), causing a brief throughput dip."

The explanation is correct but incomplete: the dip happens because BBR-SAT's **measured** bw_hi after 30 s of GEO operation is **higher** than the seeded value (BBR has been probing up). Resetting to the lower seeded ceiling clips the pacing rate. This should be stated explicitly.

**Request:** Confirm this is the correct explanation and suggest 1–2 sentences to add to the F3 paragraph.

### Issue F — Related Work Coverage

Three potential gaps a GLOBECOM reviewer might flag:

1. **Multipath QUIC for satellite** — there is recent work on MP-QUIC for satellite (e.g., Herbaut et al. 2022). Not citing it may draw comment.
2. **QUIC DATAGRAM / satellite signalling** — RFC 9221 (QUIC DATAGRAM) would be the natural cite when we mention DATAGRAM frames in §IV.
3. **LEO constellation papers** — we cite Bhattacherjee (HotNets 2019) and Michel (IMC 2022) but not Handley (HotNets 2018 "Delay is Not an Option") which is frequently cited in this space.

**Request:** Are any of these gaps significant enough to add? We have no space for new paragraphs but could add brief inline citations.

---

## Specific Revision Requests

### R1 — Fix `refs.bib` citations A–D above
Replace the four incorrect bib entries. The BBRv3 IETF draft citation is the most important — it should be:
```bibtex
@techreport{cardwell2022bbrv3,
  author      = {Cardwell, Neal and Cheng, Yuchung and others},
  title       = {{BBR} Congestion Control},
  institution = {IETF},
  type        = {Internet-Draft},
  number      = {draft-cardwell-iccrg-bbr-congestion-control-02},
  year        = {2022},
  note        = {Work in progress}
}
```
And all BBRv3-specific claims (MaxBwFilter slots, bw_hi, ProbeBW sub-states) should cite `cardwell2022bbrv3` instead of `cardwell2017`.

### R2 — F3 throughput dip explanation
In the F3 subsection, after "causing a brief throughput dip visible in both flows", add:
*"This occurs because BBR-SAT's measured \texttt{bw\_hi} after 30\,s of GEO operation has been probed above the seeded ceiling; the context-switch resets it to the conservative ephemeris value, temporarily clipping the pacing rate."*

### R3 — CUBIC single-flow closing sentence
At the end of the CUBIC paragraph in §V-A, after "behaviour under competition is discussed in §V-B", add:
*"For single-flow bulk transfer on satellite, CUBIC's loss-driven simplicity makes it competitive; BBR-SAT's advantage lies in mixed-CCA deployments where BBRv3 flows must coexist fairly."*

### R4 — Orbit table staleness: move to §VI
Remove §III.D entirely and fold the staleness note into the Limitations bullet in §VI, where it fits naturally next to the T=120 s result.

---

## Metrics Summary for Reviewer Sanity-Check

All numbers verified against raw CSV files:

| Claim | Value | Source |
|---|---|---|
| BBR-SAT T90 (LEO→GEO, ℓ=0) | 2.5 s | `exp_full/exp1_raw.csv`, baseline=4, orbit 0→2, HO=30, lead=0 |
| B1 peak queue | 33,120 KB | same row, peak_queue field |
| BBR-SAT peak queue | 910 KB | same |
| BBR-SAT goodput | 56.9 MB | same, goodput_bytes = 59,694,something |
| CUBIC goodput | 66.4 MB | same, baseline=5 |
| F1 BBRv3 J at ℓ=0 | 0.997 | `fairness/f1_sweep_run{1,2}.csv`, 2-run avg |
| F3 BBR-SAT steady | 3.43 Mbps | `fairness/f3_run{1,2}.csv`, post-t=30 avg |
| F3 CUBIC steady | 6.18 Mbps | same |
| F3 Jain J | 0.888 | same |
| F1-CUBIC J at ℓ=0 | 0.713 | `fairness/f1_cubic_sweep_run{1,2}.csv` |
| B4 T90 at ℓ=5 | 5.6 s | `exp_full/exp1_raw.csv` |
| BBR-SAT T90 at ℓ=30 | 47.6 s | same |

---

## Summary of What Opus Should Produce

1. **Corrected `refs.bib`** — fix the four wrong citations, add BBRv3 IETF draft, optionally add RFC 9221 / Handley 2018
2. **3–4 sentence edits** to paper.tex for R2, R3, R4 above
3. **Judgment calls** on Issues A–F above (keep/change/remove)
4. **Overall verdict**: is the paper ready for a workshop submission, or are there structural concerns that require a re-write of a section?
