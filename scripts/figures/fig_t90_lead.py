#!/usr/bin/env python3
"""
fig_t90_lead.py -- Figure: T90 vs lead time for LEO→GEO transition

Single panel showing T90 (seconds to 90% of new link capacity) as a function
of advance lead time for each baseline, at HO=30s.  N/C trials are plotted
as open markers at the top of the axis with a dashed "N/C" reference line.

Reads  results/exp_full/exp1_raw.csv
Writes results/fairness/fig_t90_lead.{pdf,png}
"""

import csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from pathlib import Path

IN_CSV  = Path('/home/rajat/tcpsatproject/results/capped_sweep_v1/exp1_summary.csv')
OUT_DIR = Path('/home/rajat/tcpsatproject/results/fairness')

# ── load summary (one row per condition, median T90) ──────────────────────────
with open(IN_CSV) as f:
    rows = list(csv.DictReader(f))

NC_VALUE = 65.0  # y-position for N/C markers

BASELINES = {
    0: ('B1: Vanilla BBRv3', '#d62728',  'o',  '--'),
    2: ('B3: cwnd-freeze',   '#9467bd',  's',  '--'),
    3: ('B4: pause/resume',  '#ff7f0e',  '^',  '-.' ),
    4: ('BBR-SAT (ours)',    '#1f77b4',  'D',  '-'  ),
    5: ('CUBIC',             '#2ca02c',  'v',  '-'  ),
}

def get_series(orbit_from, orbit_to, ho_time, baseline_id):
    subset = [r for r in rows
              if r['orbit_from'] == str(orbit_from)
              and r['orbit_to']  == str(orbit_to)
              and r['handover_time_s'] == str(ho_time)
              and int(r['baseline']) == baseline_id]
    subset.sort(key=lambda r: int(r['lead_time_s']))
    seen_leads = set()
    leads, t90s, nc_flags = [], [], []
    for r in subset:
        lead = int(r['lead_time_s'])
        if lead in seen_leads:
            continue
        seen_leads.add(lead)
        leads.append(lead)
        n_conv = int(r['n_converged'])
        n_runs = int(r['n_runs'])
        t90_med = float(r['t90_median_us'])
        if n_conv > 0 and t90_med > 0:
            t90s.append(t90_med / 1e6)
            nc_flags.append(False)
        else:
            t90s.append(NC_VALUE)
            nc_flags.append(True)
    return leads, t90s, nc_flags

# ── figure ────────────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(5.5, 3.6), constrained_layout=True)

NC_Y = NC_VALUE
ax.axhline(NC_Y, color='grey', ls=':', lw=0.8, alpha=0.5)
ax.text(0.5, NC_Y + 0.8, 'N/C (>60 s window)', fontsize=6.5, color='grey', va='bottom')

for bid, (label, color, marker, ls) in BASELINES.items():
    leads, t90s, nc_flags = get_series(0, 2, 30, bid)
    if not leads:
        continue

    conv_leads  = [l for l, nc in zip(leads, nc_flags) if not nc]
    conv_t90s   = [t for t, nc in zip(t90s,  nc_flags) if not nc]
    nc_leads    = [l for l, nc in zip(leads, nc_flags) if nc]
    nc_t90s     = [NC_VALUE] * len(nc_leads)

    # connected line through converged points only
    if conv_leads:
        ax.plot(conv_leads, conv_t90s, color=color, ls=ls, lw=1.8,
                marker=marker, ms=5.5, label=label, zorder=3)
    else:
        # label-only entry so legend is populated
        ax.plot([], [], color=color, ls=ls, lw=1.8, marker=marker, ms=5.5, label=label)

    # N/C markers — open, same colour
    if nc_leads:
        ax.plot(nc_leads, nc_t90s, marker=marker, ms=6, mfc='none',
                mec=color, mew=1.5, ls='none', zorder=4)

# annotation: ℓ=2 sweet spot
ax.annotate('ℓ=2 s: 1.5 s\n(3× faster)', xy=(2, 1.5), xytext=(6, 6),
            fontsize=6.5, color='#1f77b4', va='bottom',
            arrowprops=dict(arrowstyle='->', color='#1f77b4', lw=1.0))

ax.set_xlabel('Advance lead time  ℓ (s)', fontsize=9)
ax.set_ylabel('T90 (s)', fontsize=9)
ax.set_title('T90 vs lead time — LEO→GEO,  $T_{\\mathrm{HO}}=30\\,s$,  1×BDP cap', fontsize=9)

ax.set_xlim(-1, 32)
ax.set_ylim(0, NC_Y + 4)
ax.set_xticks([0, 2, 5, 10, 20, 30])
ax.tick_params(labelsize=8)
ax.legend(fontsize=7, loc='upper left', framealpha=0.9)

for ext in ('pdf', 'png'):
    out = OUT_DIR / f'fig_t90_lead.{ext}'
    fig.savefig(out, dpi=200 if ext == 'png' else None)
    print(f'Wrote {out}')
