#!/usr/bin/env python3
"""
fig_fairness.py -- Figure 4: BBR-SAT Fairness Results

Three scenarios shown (2 panels):
  (a) F1: BBR-SAT vs BBRv3 during shared LEO→GEO handover (lead=0, 2-run avg)
  (b) F3: BBR-SAT vs CUBIC on steady-state GEO

F1-CUBIC (inherited BBRv3 vs CUBIC unfairness) is noted in the caption
but shown as an inset summary bar rather than a full time-series.

Reads  results/fairness/f1_sweep_run{1,2}.csv
       results/fairness/f1_cubic_sweep_run{1,2}.csv
       results/fairness/f3_raw.csv
Writes results/fairness/fig_fairness.{pdf,png}
"""

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from pathlib import Path
from collections import defaultdict

IN_DIR  = Path('/home/rajat/tcpsatproject/results/fairness_v2')
OUT_DIR = Path('/home/rajat/tcpsatproject/results/fairness')
HANDOVER_T = 30

# ── helpers ───────────────────────────────────────────────────────────────────
def load_rows(path):
    rows = []
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) != 6: continue
            try:
                rows.append({'lead': int(p[1]), 't': int(p[2]),
                             'f1': int(p[3]), 'f2': int(p[4]), 'j': float(p[5])})
            except: pass
    return rows

def avg_runs(path1, path2):
    """Return per-(lead,t) average of two run files."""
    from collections import defaultdict
    buckets = defaultdict(list)
    for path in (path1, path2):
        for r in load_rows(path):
            buckets[(r['lead'], r['t'])].append(r)
    out = []
    for (lead, t), rs in sorted(buckets.items()):
        out.append({'lead': lead, 't': t,
                    'f1': sum(r['f1'] for r in rs)/len(rs),
                    'f2': sum(r['f2'] for r in rs)/len(rs),
                    'j':  sum(r['j']  for r in rs)/len(rs)})
    return out

# ── load ──────────────────────────────────────────────────────────────────────
f1_v3   = load_rows(IN_DIR/'f1_sweep.csv')
f1_cub  = load_rows(IN_DIR/'f1_cubic_sweep.csv')
f3_rows = sorted(load_rows(IN_DIR/'f3.csv'), key=lambda r: r['t'])

# F1 BBRv3 at lead=0
r_v3 = sorted([r for r in f1_v3 if r['lead']==0], key=lambda r: r['t'])
t_v3 = [r['t'] for r in r_v3]
sat_v3   = [r['f1']/1e6 for r in r_v3]
bbrv3    = [r['f2']/1e6 for r in r_v3]
jain_v3  = [r['j'] for r in r_v3]

# F3
t_f3 = [r['t'] for r in f3_rows]
sat_f3   = [r['f1']/1e6 for r in f3_rows]
cub_f3   = [r['f2']/1e6 for r in f3_rows]
jain_f3  = [r['j'] for r in f3_rows]

# F1-CUBIC summary (2-run avg post-handover) by lead
f1_cub_by_lead = defaultdict(list)
for r in f1_cub:
    if r['t'] > HANDOVER_T:
        f1_cub_by_lead[r['lead']].append(r)
cub_leads = sorted(f1_cub_by_lead.keys())
cub_sat_means = [np.mean([r['f1']/1e6 for r in f1_cub_by_lead[l]]) for l in cub_leads]
cub_cub_means = [np.mean([r['f2']/1e6 for r in f1_cub_by_lead[l]]) for l in cub_leads]

# ── colours ───────────────────────────────────────────────────────────────────
BBRSAT_COL = '#1f77b4'
BBRV3_COL  = '#d62728'
CUBIC_COL  = '#ff7f0e'
JAIN_COL   = '#8c564b'

# ── figure: 1×2 panels ───────────────────────────────────────────────────────
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.16, 3.2), constrained_layout=True)

# ── Panel (a): F1 — BBR-SAT vs BBRv3, lead=0, 2-run average ─────────────────
ax1.plot(t_v3, sat_v3,  color=BBRSAT_COL, lw=1.6, label='BBR-SAT (flow 1)')
ax1.plot(t_v3, bbrv3,   color=BBRV3_COL,  lw=1.4, ls='--', label='BBRv3 (flow 2)')

ax1.axhline(5, color='grey', ls=':', lw=0.8, alpha=0.4)
ax1.text(1, 5.2, '5 Mbps fair share (LEO)', fontsize=6, color='grey')
ax1.axhline(1.5,  color='grey', ls=':', lw=0.8, alpha=0.5)
ax1.text(1,  1.65, '1.5 Mbps fair share (GEO)',  fontsize=6, color='grey')

ax1.axvline(HANDOVER_T, color='black', ls=':', lw=0.9, alpha=0.7)
ax1.text(HANDOVER_T + 0.7, 6.2, 'handover', fontsize=6.5, color='black', alpha=0.8)

post_v3 = [r for r in r_v3 if r['t'] > HANDOVER_T]
j_v3 = np.mean([r['j'] for r in post_v3])
ax1.text(50, 0.3, f'Post-handover Jain J̄={j_v3:.3f}', fontsize=6.5,
         style='italic',
         bbox=dict(boxstyle='round,pad=0.2', fc='lightyellow', ec='grey', alpha=0.9))

# F1-CUBIC inset — stacked bar at top-right
ax_ins = ax1.inset_axes([0.63, 0.55, 0.35, 0.38])
leads_label = [str(l) for l in cub_leads]
x = np.arange(len(cub_leads))
ax_ins.bar(x, cub_sat_means, color=BBRSAT_COL, alpha=0.85, width=0.6, label='BBR-SAT')
ax_ins.bar(x, cub_cub_means, bottom=cub_sat_means, color=CUBIC_COL, alpha=0.85, width=0.6, label='CUBIC')
ax_ins.set_xticks(x); ax_ins.set_xticklabels(leads_label, fontsize=5)
ax_ins.set_xlabel('lead (s)', fontsize=5.5); ax_ins.set_ylabel('Mbps', fontsize=5.5)
ax_ins.set_title('F1 vs CUBIC', fontsize=5.5)
ax_ins.tick_params(labelsize=5); ax_ins.set_ylim(0, 4)
ax_ins.axhline(1.5, color='grey', ls=':', lw=0.6)

ax1.set_xlabel('Simulated time (s)', fontsize=8)
ax1.set_ylabel('Throughput (Mbps)', fontsize=8)
ax1.set_title('(a) F1: Shared LEO→GEO handover (lead=0, 2-run avg)', fontsize=8)
ax1.set_ylim(0, 7); ax1.set_xlim(0, 90)
ax1.tick_params(labelsize=7)
ax1.legend(fontsize=6.5, loc='upper left')

# ── Panel (b): F3 — BBR-SAT vs CUBIC, steady-state GEO ──────────────────────
ax2b = ax2.twinx()

ax2.plot(t_f3, sat_f3, color=BBRSAT_COL, lw=1.6, label='BBR-SAT')
ax2.plot(t_f3, cub_f3, color=CUBIC_COL,  lw=1.4, ls='--', label='CUBIC')
ax2.axhline(1.5, color='grey', ls=':', lw=0.8, alpha=0.5)
ax2.text(1, 1.65, '1.5 Mbps fair share', fontsize=6, color='grey')

ax2b.plot(t_f3, jain_f3, color=JAIN_COL, lw=0.9, ls=':', alpha=0.8, label="Jain's J")
ax2b.axhline(0.9, color=JAIN_COL, ls='--', lw=0.6, alpha=0.4)
ax2b.set_ylabel("Jain's fairness index", fontsize=7, color=JAIN_COL)
ax2b.tick_params(axis='y', labelsize=6, colors=JAIN_COL)
ax2b.set_ylim(0.3, 1.05)

ax2.axvline(HANDOVER_T, color='black', ls=':', lw=0.9, alpha=0.7)
ax2.text(HANDOVER_T + 0.7, 5.0, 'GEO signal', fontsize=6.5, color='black', alpha=0.8)

post_f3 = [r for r in f3_rows if r['t'] > HANDOVER_T]
j_f3 = np.mean([r['j'] for r in post_f3])
ax2.text(35, 0.3, f'Post-signal Jain J̄={j_f3:.3f}', fontsize=6.5,
         style='italic',
         bbox=dict(boxstyle='round,pad=0.2', fc='lightyellow', ec='grey', alpha=0.9))

lines1, labs1 = ax2.get_legend_handles_labels()
lines2, labs2 = ax2b.get_legend_handles_labels()
ax2.legend(lines1 + lines2, labs1 + labs2, fontsize=6, loc='lower right')

ax2.set_xlabel('Simulated time (s)', fontsize=8)
ax2.set_ylabel('Throughput (Mbps)', fontsize=8)
ax2.set_title('(b) F3: BBR-SAT vs CUBIC — steady-state GEO (2-run avg)', fontsize=8)
ax2.set_ylim(0, 6); ax2.set_xlim(0, 90)
ax2.tick_params(labelsize=7)

# ── save ─────────────────────────────────────────────────────────────────────
for ext in ('pdf', 'png'):
    out = OUT_DIR / f'fig_fairness.{ext}'
    fig.savefig(out, dpi=200 if ext == 'png' else None)
    print(f'Wrote {out}')

# ── summary table ─────────────────────────────────────────────────────────────
print('\n=== Fairness summary (post-handover / post-signal) ===')
print(f'F1 BBR-SAT vs BBRv3  (lead=0, 2-run avg): SAT={np.mean([r["f1"]/1e6 for r in post_v3]):.2f}  v3={np.mean([r["f2"]/1e6 for r in post_v3]):.2f}  J={j_v3:.4f}')
for lead in [0, 5, 10, 20]:
    rows = f1_cub_by_lead.get(lead, [])
    if not rows: continue
    s = np.mean([r['f1']/1e6 for r in rows]); c = np.mean([r['f2']/1e6 for r in rows]); j = np.mean([r['j'] for r in rows])
    print(f'F1 BBR-SAT vs CUBIC  (lead={lead:2d}, 2-run avg): SAT={s:.2f}  CUBIC={c:.2f}  J={j:.4f}')
s3 = np.mean([r['f1']/1e6 for r in post_f3]); c3 = np.mean([r['f2']/1e6 for r in post_f3])
print(f'F3 BBR-SAT vs CUBIC  (steady GEO, 2-run avg):  SAT={s3:.2f}  CUBIC={c3:.2f}  J={j_f3:.4f}')
