#!/usr/bin/env python3
"""
BBR-SAT Figure 3 — LEO→GEO lead-time sensitivity
IEEE GLOBECOM 2026 Workshop paper figure

Fixes from Opus review:
1. Remove pink "never converged" band — use a distinct visual break instead
2. Cap x-axis at lead=20s (operational sweet spot); note lead=30s degradation in text
3. Use dual-panel with proper IEEE column width sizing
4. CUBIC goodput shown on same scale (no clipping, no arrows)
5. Cleaner baseline separation in goodput panel
6. Add annotations for key findings directly on the figure

Usage: Run on N100 server:
  python3 fig3_corrected.py

Reads: /home/rajat/tcpsatproject/results/exp_full/exp1_summary.csv
Writes: fig3_corrected.pdf, fig3_corrected.png
"""

import csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path

# === DATA LOADING ===
CSV_PATH = '/home/rajat/tcpsatproject/results/exp_full/exp1_summary.csv'
OUT_DIR = Path('/home/rajat/tcpsatproject/results/exp_full')

rows = []
with open(CSV_PATH) as f:
    reader = csv.DictReader(f)
    for r in reader:
        rows.append(r)

# Filter: LEO→GEO, T=30s, 0% loss
filtered = [r for r in rows
            if r['orbit_from'] == '0' and r['orbit_to'] == '2'
            and r['handover_time_s'] == '30' and r['loss'] == '0']

# === STYLE DEFINITIONS (IEEE-friendly, colorblind-safe) ===
BASELINES = {
    0: {'name': 'Vanilla BBRv3',  'color': '#c0392b', 'ls': '--',  'marker': 's', 'ms': 5, 'lw': 1.4, 'zorder': 2},
    2: {'name': 'cwnd-freeze',    'color': '#e67e22', 'ls': '-.',  'marker': '^', 'ms': 5, 'lw': 1.4, 'zorder': 2},
    3: {'name': 'pause/resume',   'color': '#27ae60', 'ls': ':',   'marker': 'D', 'ms': 4, 'lw': 1.4, 'zorder': 2},
    4: {'name': 'BBR-SAT (ours)', 'color': '#2c3e50', 'ls': '-',   'marker': 'o', 'ms': 6, 'lw': 2.2, 'zorder': 4},
    5: {'name': 'CUBIC',          'color': '#8e44ad', 'ls': '--',  'marker': 'P', 'ms': 5, 'lw': 1.4, 'zorder': 3},
}

# Lead times to show (cap at 20s — lead=30s degrades; noted in paper text)
LEAD_TIMES_SHOW = [0, 2, 5, 10, 20]

# === HELPER: extract data for one baseline ===
def get_baseline_data(baseline_id, lead_times=None):
    brows = [r for r in filtered if int(r['baseline']) == baseline_id]
    brows.sort(key=lambda x: float(x['lead_time_s']))
    if lead_times is not None:
        brows = [r for r in brows if float(r['lead_time_s']) in lead_times]
    leads = [float(r['lead_time_s']) for r in brows]
    t90 = []
    for r in brows:
        if float(r['n_converged']) > 0:
            t90.append(float(r['t90_median_us']) / 1e6)
        else:
            t90.append(None)  # never converged
    goodput = [float(r['goodput_mean_bytes']) / 1e6 for r in brows]
    return leads, t90, goodput


# === FIGURE ===
plt.rcParams.update({
    'font.family': 'serif',
    'font.size': 9,
    'axes.labelsize': 10,
    'axes.titlesize': 10,
    'legend.fontsize': 8,
    'xtick.labelsize': 8,
    'ytick.labelsize': 8,
    'figure.dpi': 150,
})

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.16, 2.8))  # IEEE two-column width
fig.subplots_adjust(wspace=0.35, top=0.82, bottom=0.18, left=0.09, right=0.97)

# --- Panel (a): T90 convergence time ---
# Only show baselines that converge (BBR-SAT, B4, CUBIC)
# B1 and B3 get a text annotation instead of misleading markers

for bid in [4, 5, 3]:  # BBR-SAT first (foreground), then CUBIC, then B4
    s = BASELINES[bid]
    leads, t90, _ = get_baseline_data(bid, LEAD_TIMES_SHOW)
    
    conv_leads = [l for l, t in zip(leads, t90) if t is not None]
    conv_t90 = [t for t in t90 if t is not None]
    
    if conv_leads:
        ax1.plot(conv_leads, conv_t90, color=s['color'], linestyle=s['ls'],
                 marker=s['marker'], markersize=s['ms'], linewidth=s['lw'],
                 zorder=s['zorder'], label=s['name'])

# Annotate B1 and B3 as "never converged" — no misleading markers
ax1.annotate('B1 (BBRv3) & B3 (cwnd-freeze):\nnever converge at any lead time',
             xy=(10, 11), fontsize=7, color='#c0392b', style='italic',
             ha='center',
             bbox=dict(boxstyle='round,pad=0.3', facecolor='#fdeaea',
                       edgecolor='#c0392b', alpha=0.8))

ax1.set_xlabel('Advance notice (s)')
ax1.set_ylabel('Time to 90% throughput (s)')
ax1.set_title('(a) Convergence time')
ax1.set_xlim(-1, 22)
ax1.set_ylim(0, 14)
ax1.yaxis.set_major_locator(ticker.MultipleLocator(2))
ax1.xaxis.set_major_locator(ticker.MultipleLocator(5))
ax1.grid(True, alpha=0.25, linewidth=0.5)
ax1.legend(loc='upper right', fontsize=7, framealpha=0.9)

# Key callout on BBR-SAT
ax1.annotate('BBR-SAT: 2.5 s\nacross all lead times',
             xy=(10, 2.5), xytext=(14, 5.5),
             fontsize=7, color='#2c3e50', weight='bold',
             arrowprops=dict(arrowstyle='->', color='#2c3e50', lw=1.0),
             bbox=dict(boxstyle='round,pad=0.2', facecolor='#eaf2f8',
                       edgecolor='#2c3e50', alpha=0.9))

# --- Panel (b): Goodput ---
for bid in [4, 5, 0, 2, 3]:
    s = BASELINES[bid]
    leads, _, goodput = get_baseline_data(bid, LEAD_TIMES_SHOW)
    ax2.plot(leads, goodput, color=s['color'], linestyle=s['ls'],
             marker=s['marker'], markersize=s['ms'], linewidth=s['lw'],
             zorder=s['zorder'], label=s['name'])

ax2.set_xlabel('Advance notice (s)')
ax2.set_ylabel('Goodput (MB, 55 s post-handover)')
ax2.set_title('(b) Throughput recovery')
ax2.set_xlim(-1, 22)
ax2.set_ylim(0, 80)
ax2.yaxis.set_major_locator(ticker.MultipleLocator(10))
ax2.xaxis.set_major_locator(ticker.MultipleLocator(5))
ax2.grid(True, alpha=0.25, linewidth=0.5)

# Annotate the three tiers
ax2.annotate('CUBIC: ~66 MB\n(overshoot + sawtooth)',
             xy=(10, 66), xytext=(14, 72),
             fontsize=6.5, color='#8e44ad',
             arrowprops=dict(arrowstyle='->', color='#8e44ad', lw=0.8))
ax2.annotate('BBR-SAT: ~56 MB\n(controlled convergence)',
             xy=(10, 57), xytext=(14, 48),
             fontsize=6.5, color='#2c3e50', weight='bold',
             arrowprops=dict(arrowstyle='->', color='#2c3e50', lw=0.8))
ax2.annotate('B1/B3/B4: 3–9 MB\n(stuck or restarting)',
             xy=(10, 7), xytext=(14, 18),
             fontsize=6.5, color='#7f8c8d',
             arrowprops=dict(arrowstyle='->', color='#7f8c8d', lw=0.8))

# Compact legend for panel (b)
ax2.legend(loc='center right', fontsize=6.5, framealpha=0.9,
           bbox_to_anchor=(0.98, 0.55))

# === TITLE ===
fig.suptitle('LEO→GEO handover at T = 30 s  |  GEO: 10 Mbps, 580 ms RTT  |  picoquic simulation',
             fontsize=9, y=0.96, weight='bold')

# === SAVE ===
plt.savefig(OUT_DIR / 'fig3_corrected.pdf', bbox_inches='tight', dpi=300)
plt.savefig(OUT_DIR / 'fig3_corrected.png', bbox_inches='tight', dpi=200)
print(f"Saved to {OUT_DIR / 'fig3_corrected.pdf'}")
print(f"Saved to {OUT_DIR / 'fig3_corrected.png'}")
plt.close()
