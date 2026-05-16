#!/usr/bin/env python3
"""
BBR-SAT Figure 3 v4 — clean, no text overlap, shared top legend
Run: python3 fig3_v4.py
"""
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.lines as mlines
from pathlib import Path

CSV_PATH = '/home/rajat/tcpsatproject/results/exp_full/exp1_summary.csv'
OUT_DIR = Path('/home/rajat/tcpsatproject/results/exp_full')

rows = []
with open(CSV_PATH) as f:
    for r in csv.DictReader(f):
        rows.append(r)

filtered = [r for r in rows
            if r['orbit_from']=='0' and r['orbit_to']=='2'
            and r['handover_time_s']=='30' and r['loss']=='0']

BASELINES = {
    0: {'name': 'Vanilla BBRv3',  'color': '#c0392b', 'ls': '--',  'marker': 's', 'ms': 5, 'lw': 1.4, 'zo': 2},
    2: {'name': 'cwnd-freeze',    'color': '#e67e22', 'ls': '-.',  'marker': '^', 'ms': 5, 'lw': 1.4, 'zo': 2},
    3: {'name': 'pause/resume',   'color': '#27ae60', 'ls': ':',   'marker': 'D', 'ms': 4, 'lw': 1.4, 'zo': 2},
    4: {'name': 'BBR-SAT (ours)', 'color': '#2c3e50', 'ls': '-',   'marker': 'o', 'ms': 6, 'lw': 2.2, 'zo': 4},
    5: {'name': 'CUBIC',          'color': '#8e44ad', 'ls': '--',  'marker': 'P', 'ms': 5, 'lw': 1.4, 'zo': 3},
}
LEADS = [0, 2, 5, 10, 20]

def get_data(bid):
    brows = sorted([r for r in filtered if int(r['baseline'])==bid],
                   key=lambda x: float(x['lead_time_s']))
    brows = [r for r in brows if float(r['lead_time_s']) in LEADS]
    leads = [float(r['lead_time_s']) for r in brows]
    t90 = []
    for r in brows:
        if float(r['n_converged']) > 0:
            t90.append(float(r['t90_median_us']) / 1e6)
        else:
            t90.append(None)
    goodput = [float(r['goodput_mean_bytes']) / 1e6 for r in brows]
    return leads, t90, goodput

# === FIGURE SETUP ===
plt.rcParams.update({
    'font.family': 'serif',
    'font.size': 9,
    'axes.labelsize': 10,
    'axes.titlesize': 10,
    'xtick.labelsize': 8,
    'ytick.labelsize': 8,
    'figure.dpi': 150,
})

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.16, 3.5))

# === SHARED TOP LEGEND ===
handles = []
for bid in [0, 2, 3, 4, 5]:
    s = BASELINES[bid]
    h = mlines.Line2D([], [], color=s['color'], linestyle=s['ls'],
                      marker=s['marker'], markersize=s['ms'],
                      linewidth=s['lw'], label=s['name'])
    handles.append(h)

fig.legend(handles=handles, loc='upper center',
           bbox_to_anchor=(0.5, 0.99), ncol=5,
           fontsize=8, framealpha=0.95, edgecolor='#cccccc',
           columnspacing=1.2, handlelength=2.5, handletextpad=0.5)

fig.suptitle('LEO\u2192GEO handover at T = 30 s  |  GEO: 10 Mbps, 580 ms RTT  |  picoquic simulation',
             fontsize=9, y=1.06, weight='bold')

# === PANEL (a): T90 ===
# Plot only baselines that converge: BBR-SAT, CUBIC, B4
for bid in [4, 5, 3]:
    s = BASELINES[bid]
    leads, t90, _ = get_data(bid)
    conv_l = [l for l, t in zip(leads, t90) if t is not None]
    conv_t = [t for t in t90 if t is not None]
    if conv_l:
        ax1.plot(conv_l, conv_t, color=s['color'], linestyle=s['ls'],
                 marker=s['marker'], markersize=s['ms'], linewidth=s['lw'],
                 zorder=s['zo'])

# B1 and B3: gray band at top indicating "never converges"
ax1.axhspan(12.5, 14, color='#f5f5f5', zorder=0)
ax1.text(10, 13.25, 'BBRv3 & cwnd-freeze: never converge',
         ha='center', va='center', fontsize=7, color='#999999', style='italic')

ax1.set_xlabel('Advance notice (s)')
ax1.set_ylabel('Time to 90% throughput (s)')
ax1.set_title('(a) Convergence time', pad=8)
ax1.set_xlim(-1, 22)
ax1.set_ylim(0, 14.5)
ax1.yaxis.set_major_locator(ticker.MultipleLocator(2))
ax1.xaxis.set_major_locator(ticker.MultipleLocator(5))
ax1.grid(True, alpha=0.2, linewidth=0.5)

# === PANEL (b): Goodput ===
for bid in [5, 4, 0, 2, 3]:
    s = BASELINES[bid]
    leads, _, goodput = get_data(bid)
    ax2.plot(leads, goodput, color=s['color'], linestyle=s['ls'],
             marker=s['marker'], markersize=s['ms'], linewidth=s['lw'],
             zorder=s['zo'])

ax2.set_xlabel('Advance notice (s)')
ax2.set_ylabel('Goodput (MB, 55 s post-handover)')
ax2.set_title('(b) Throughput recovery', pad=8)
ax2.set_xlim(-1, 22)
ax2.set_ylim(0, 80)
ax2.yaxis.set_major_locator(ticker.MultipleLocator(10))
ax2.xaxis.set_major_locator(ticker.MultipleLocator(5))
ax2.grid(True, alpha=0.2, linewidth=0.5)

# === LAYOUT ===
fig.subplots_adjust(wspace=0.38, top=0.85, bottom=0.14, left=0.09, right=0.97)

# === SAVE ===
plt.savefig(OUT_DIR / 'fig3_v4.pdf', bbox_inches='tight', dpi=300)
plt.savefig(OUT_DIR / 'fig3_v4.png', bbox_inches='tight', dpi=200)
print(f"Saved to {OUT_DIR}/fig3_v4.pdf and .png")
plt.close()
