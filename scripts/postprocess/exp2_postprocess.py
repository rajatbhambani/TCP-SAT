#!/usr/bin/env python3
"""
exp2_postprocess.py -- Post-processing for Experiment 2 (fairness).

Reads 2-run CSV data from results/fairness/, produces publication figures
and a summary CSV in results/exp2/.

Figures produced
----------------
fig_exp2_f1.pdf/png   -- F1: per-flow throughput + Jain's J (BBR-SAT vs BBRv3)
fig_exp2_f3.pdf/png   -- F3: per-flow throughput + Jain's J (BBR-SAT vs CUBIC)
fig_exp2_jains.pdf/png -- Jain's J by lead time for all three scenarios
exp2_summary.csv       -- numerical summary table

Input CSV format (all files)
-----------------------------
scenario,lead_time_s,t_s,flow1_rate_bps,flow2_rate_bps[,jains_index]
"""

import sys
import csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path
from collections import defaultdict

# ── paths ─────────────────────────────────────────────────────────────────────
IN_DIR  = Path('/home/rajat/tcpsatproject/results/fairness')
OUT_DIR = Path('/home/rajat/tcpsatproject/results/exp2')
OUT_DIR.mkdir(parents=True, exist_ok=True)

HANDOVER_T = 30   # seconds

# ── IEEE figure geometry ──────────────────────────────────────────────────────
COL1_W = 3.5    # single column width (inches)
COL2_W = 7.16   # double column width (inches)
FIG_H  = 2.8    # panel height (inches)

# ── colour palette (colorblind-safe) ─────────────────────────────────────────
C_BBRSAT = '#1f77b4'   # blue
C_BBRV3  = '#d62728'   # red
C_CUBIC  = '#ff7f0e'   # orange
C_JAIN   = '#2ca02c'   # green  (Jain's index)
C_VLINE  = '#555555'   # event marker

LEAD_COLORS = {
    0:  '#1f77b4',
    5:  '#2ca02c',
    10: '#9467bd',
    15: '#8c564b',
    20: '#2c3e50',
    30: '#7f7f7f',
}

# ── data loading ──────────────────────────────────────────────────────────────
def load_rows(path):
    rows = []
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) < 5:
                continue
            try:
                rows.append({
                    'scenario': p[0],
                    'lead':     int(p[1]),
                    't':        int(p[2]),
                    'f1':       int(p[3]),
                    'f2':       int(p[4]),
                    'j':        float(p[5]) if len(p) > 5 else None,
                })
            except ValueError:
                continue
    return rows


def avg_runs(*paths):
    """Average multiple run files at the (lead, t) level."""
    buckets = defaultdict(list)
    for path in paths:
        for r in load_rows(path):
            buckets[(r['lead'], r['t'])].append(r)
    out = []
    for (lead, t), rs in sorted(buckets.items()):
        js = [r['j'] for r in rs if r['j'] is not None]
        out.append({
            'lead': lead,
            't':    t,
            'f1':   np.mean([r['f1'] for r in rs]),
            'f2':   np.mean([r['f2'] for r in rs]),
            'j':    np.mean(js) if js else None,
        })
    return out


def post_ho(rows, lead=None, t_min=HANDOVER_T):
    """Filter to post-handover rows (optionally for one lead time)."""
    r = [x for x in rows if x['t'] > t_min]
    if lead is not None:
        r = [x for x in r if x['lead'] == lead]
    return r


def summary_stats(rows):
    """Return (sat_mean, opp_mean, j_mean, total_mean) in Mbps / index."""
    if not rows:
        return 0., 0., 0., 0.
    sat  = np.mean([r['f1'] for r in rows]) / 1e6
    opp  = np.mean([r['f2'] for r in rows]) / 1e6
    js   = [r['j'] for r in rows if r['j'] is not None]
    j    = np.mean(js) if js else float('nan')
    return sat, opp, j, sat + opp


# ── load all datasets ─────────────────────────────────────────────────────────
f1_v3  = avg_runs(IN_DIR / 'f1_sweep_run1.csv',
                  IN_DIR / 'f1_sweep_run2.csv')

f1_cub = avg_runs(IN_DIR / 'f1_cubic_sweep_run1.csv',
                  IN_DIR / 'f1_cubic_sweep_run2.csv')

f3     = avg_runs(IN_DIR / 'f3_run1.csv',
                  IN_DIR / 'f3_run2.csv')

# ── helper: pick one lead time as a time-series ───────────────────────────────
def timeseries(rows, lead):
    r = sorted([x for x in rows if x['lead'] == lead], key=lambda x: x['t'])
    return (
        np.array([x['t']          for x in r]),
        np.array([x['f1'] / 1e6   for x in r]),
        np.array([x['f2'] / 1e6   for x in r]),
        np.array([x['j']          for x in r if x['j'] is not None]),
    )


# ══════════════════════════════════════════════════════════════════════════════
# Figure 1 — F1: BBR-SAT vs BBRv3, lead=0 (time series + Jain's)
# ══════════════════════════════════════════════════════════════════════════════
t, sat, v3, jv3 = timeseries(f1_v3, lead=0)

fig, (ax_r, ax_j) = plt.subplots(2, 1, figsize=(COL1_W, FIG_H * 1.35),
                                   sharex=True, constrained_layout=True,
                                   gridspec_kw={'height_ratios': [2, 1]})

ax_r.plot(t, sat, color=C_BBRSAT, lw=1.6, label='BBR-SAT')
ax_r.plot(t, v3,  color=C_BBRV3,  lw=1.4, ls='--', label='BBRv3')
ax_r.axhline(25, color='grey', ls=':', lw=0.7, alpha=0.5)
ax_r.text(1, 25.7, '25 Mbps (LEO fair share)', fontsize=6, color='grey')
ax_r.axhline(5,  color='grey', ls=':', lw=0.7, alpha=0.6)
ax_r.text(1,  5.6, '5 Mbps (GEO fair share)',  fontsize=6, color='grey')
ax_r.axvline(HANDOVER_T, color=C_VLINE, ls=':', lw=0.9)
ax_r.set_ylabel('Throughput (Mbps)', fontsize=8)
ax_r.set_ylim(0, 54)
ax_r.legend(fontsize=7, loc='upper right', framealpha=0.8)
ax_r.tick_params(labelsize=7)

j_post_mean = np.mean([r['j'] for r in post_ho(f1_v3, lead=0)])
ax_j.plot(t[:len(jv3)], jv3, color=C_JAIN, lw=1.2)
ax_j.axhline(1.0, color='grey', ls=':', lw=0.7, alpha=0.5)
ax_j.axhline(0.9, color='grey', ls=':', lw=0.7, alpha=0.4)
ax_j.axvline(HANDOVER_T, color=C_VLINE, ls=':', lw=0.9)
ax_j.text(32, 0.25, f'J̄ = {j_post_mean:.3f} (post-HO)', fontsize=6.5, color=C_JAIN)
ax_j.set_xlabel('Simulated time (s)', fontsize=8)
ax_j.set_ylabel("Jain's J", fontsize=8)
ax_j.set_ylim(0.0, 1.05)
ax_j.set_xlim(0, 90)
ax_j.tick_params(labelsize=7)
ax_j.text(HANDOVER_T + 0.5, 0.15, 'handover', fontsize=6, color=C_VLINE, rotation=90, va='bottom')

fig.suptitle('F1: BBR-SAT vs BBRv3 — shared LEO→GEO handover (lead=0)', fontsize=8)

for ext in ('pdf', 'png'):
    fig.savefig(OUT_DIR / f'fig_exp2_f1.{ext}', dpi=200 if ext == 'png' else None)
plt.close(fig)
print('Wrote fig_exp2_f1.{pdf,png}')


# ══════════════════════════════════════════════════════════════════════════════
# Figure 2 — F3: BBR-SAT vs CUBIC, steady-state GEO (time series + Jain's)
# ══════════════════════════════════════════════════════════════════════════════
t3, sat3, cub3, jf3 = timeseries(f3, lead=0)

fig, (ax_r, ax_j) = plt.subplots(2, 1, figsize=(COL1_W, FIG_H * 1.35),
                                   sharex=True, constrained_layout=True,
                                   gridspec_kw={'height_ratios': [2, 1]})

ax_r.plot(t3, sat3, color=C_BBRSAT, lw=1.6, label='BBR-SAT')
ax_r.plot(t3, cub3, color=C_CUBIC,  lw=1.4, ls='--', label='CUBIC')
ax_r.axhline(5, color='grey', ls=':', lw=0.7, alpha=0.6)
ax_r.text(1, 5.5, '5 Mbps (fair share)', fontsize=6, color='grey')
ax_r.axvline(HANDOVER_T, color=C_VLINE, ls=':', lw=0.9)
ax_r.set_ylabel('Throughput (Mbps)', fontsize=8)
ax_r.set_ylim(0, 22)
ax_r.legend(fontsize=7, loc='upper right', framealpha=0.8)
ax_r.tick_params(labelsize=7)

j3_post_mean = np.mean([r['j'] for r in post_ho(f3, lead=0)])
sat3_mean, cub3_mean, _, _ = summary_stats(post_ho(f3, lead=0))
ax_r.text(35, 0.7,
          f'BBR-SAT: {sat3_mean:.1f} Mbps\nCUBIC: {cub3_mean:.1f} Mbps',
          fontsize=6.5, color='black',
          bbox=dict(boxstyle='round,pad=0.2', fc='lightyellow', ec='grey', alpha=0.9))

ax_j.plot(t3[:len(jf3)], jf3, color=C_JAIN, lw=1.2)
ax_j.axhline(1.0, color='grey', ls=':', lw=0.7, alpha=0.5)
ax_j.axhline(0.9, color='grey', ls=':', lw=0.7, alpha=0.4)
ax_j.axhline(0.9, color='grey', ls='--', lw=0.6, alpha=0.3)
ax_j.axvline(HANDOVER_T, color=C_VLINE, ls=':', lw=0.9)
ax_j.text(32, 0.25, f'J̄ = {j3_post_mean:.3f} (post-signal)', fontsize=6.5, color=C_JAIN)
ax_j.set_xlabel('Simulated time (s)', fontsize=8)
ax_j.set_ylabel("Jain's J", fontsize=8)
ax_j.set_ylim(0.0, 1.05)
ax_j.set_xlim(0, 90)
ax_j.tick_params(labelsize=7)
ax_j.text(HANDOVER_T + 0.5, 0.15, 'GEO signal', fontsize=6, color=C_VLINE, rotation=90, va='bottom')

fig.suptitle('F3: BBR-SAT vs CUBIC — steady-state GEO (2-run avg)', fontsize=8)

for ext in ('pdf', 'png'):
    fig.savefig(OUT_DIR / f'fig_exp2_f3.{ext}', dpi=200 if ext == 'png' else None)
plt.close(fig)
print('Wrote fig_exp2_f3.{pdf,png}')


# ══════════════════════════════════════════════════════════════════════════════
# Figure 3 — Jain's J vs lead time: F1 BBRv3, F1 CUBIC, F3 CUBIC (bar chart)
# ══════════════════════════════════════════════════════════════════════════════
leads_all = sorted(set(r['lead'] for r in f1_v3))

j_v3_by_lead  = [np.mean([r['j'] for r in post_ho(f1_v3,  lead=l)]) for l in leads_all]
j_cub_by_lead = [np.mean([r['j'] for r in post_ho(f1_cub, lead=l)]) for l in leads_all]
j_f3_scalar   = np.mean([r['j'] for r in post_ho(f3, lead=0)])

fig, ax = plt.subplots(figsize=(COL1_W, FIG_H), constrained_layout=True)

x = np.arange(len(leads_all))
w = 0.32
bars_v3  = ax.bar(x - w/2, j_v3_by_lead,  width=w, color=C_BBRV3,  alpha=0.85,
                  label='F1: vs BBRv3')
bars_cub = ax.bar(x + w/2, j_cub_by_lead, width=w, color=C_CUBIC,  alpha=0.85,
                  label='F1: vs CUBIC')

# F3 horizontal reference (single lead=0 value)
ax.axhline(j_f3_scalar, color=C_BBRSAT, ls='--', lw=1.2,
           label=f'F3: vs CUBIC (GEO steady-state, J={j_f3_scalar:.3f})')

ax.axhline(1.0, color='grey', ls=':', lw=0.7, alpha=0.4)
ax.axhline(0.9, color='grey', ls=':', lw=0.7, alpha=0.4)
ax.text(len(leads_all) - 0.4, 0.915, '0.9', fontsize=6, color='grey', va='bottom')

ax.set_xticks(x)
ax.set_xticklabels([str(l) for l in leads_all], fontsize=7)
ax.set_xlabel('Lead time (s)', fontsize=8)
ax.set_ylabel("Jain's fairness index (J)", fontsize=8)
ax.set_title("Jain's J vs lead time — Experiment 2", fontsize=8)
ax.set_ylim(0.5, 1.05)
ax.legend(fontsize=6.5, loc='lower right', framealpha=0.9)
ax.tick_params(labelsize=7)

# Annotate bar values
for bar in list(bars_v3) + list(bars_cub):
    h = bar.get_height()
    ax.text(bar.get_x() + bar.get_width()/2, h + 0.006, f'{h:.2f}',
            ha='center', va='bottom', fontsize=5.5)

for ext in ('pdf', 'png'):
    fig.savefig(OUT_DIR / f'fig_exp2_jains.{ext}', dpi=200 if ext == 'png' else None)
plt.close(fig)
print('Wrote fig_exp2_jains.{pdf,png}')


# ══════════════════════════════════════════════════════════════════════════════
# Summary CSV
# ══════════════════════════════════════════════════════════════════════════════
summary_rows = []

for lead in leads_all:
    s, v, j, tot = summary_stats(post_ho(f1_v3,  lead=lead))
    summary_rows.append({'scenario': 'F1_vs_BBRv3', 'lead_s': lead,
                         'sat_mbps': round(s,2), 'opp_mbps': round(v,2),
                         'opp_cc': 'BBRv3', 'jains': round(j,4),
                         'total_mbps': round(tot,2), 'n_runs': 2})

for lead in leads_all:
    s, c, j, tot = summary_stats(post_ho(f1_cub, lead=lead))
    summary_rows.append({'scenario': 'F1_vs_CUBIC', 'lead_s': lead,
                         'sat_mbps': round(s,2), 'opp_mbps': round(c,2),
                         'opp_cc': 'CUBIC', 'jains': round(j,4),
                         'total_mbps': round(tot,2), 'n_runs': 2})

s3, c3, j3, tot3 = summary_stats(post_ho(f3, lead=0))
summary_rows.append({'scenario': 'F3_vs_CUBIC', 'lead_s': 0,
                     'sat_mbps': round(s3,2), 'opp_mbps': round(c3,2),
                     'opp_cc': 'CUBIC', 'jains': round(j3,4),
                     'total_mbps': round(tot3,2), 'n_runs': 2})

summary_path = OUT_DIR / 'exp2_summary.csv'
with open(summary_path, 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=['scenario','lead_s','sat_mbps','opp_mbps',
                                       'opp_cc','jains','total_mbps','n_runs'])
    w.writeheader()
    w.writerows(summary_rows)
print(f'Wrote exp2_summary.csv')

# ── console table ─────────────────────────────────────────────────────────────
print()
print(f"{'Scenario':<18} {'lead':>4}  {'BBR-SAT':>8} {'Opponent':>9} {'CC':>6}  {'Jain J':>7}  {'Total':>7}")
print('-' * 72)
for r in summary_rows:
    print(f"{r['scenario']:<18} {r['lead_s']:>4}  "
          f"{r['sat_mbps']:>7.2f}M {r['opp_mbps']:>8.2f}M {r['opp_cc']:>6}  "
          f"{r['jains']:>7.4f}  {r['total_mbps']:>6.2f}M")
