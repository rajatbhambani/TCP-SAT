#!/usr/bin/env python3
"""
plot_lead_time_curve.py -- Figure 3: T90 vs lead_time for all baselines.
Most important figure in the paper. Shows the value of advance notice.

Usage:
    python3 plot_lead_time_curve.py results/exp1/exp1_summary.csv

Output:
    results/exp1/fig3_t90_vs_lead_time.pdf
    results/exp1/fig3_t90_vs_lead_time.png
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

BASELINE_NAMES = {
    0: 'B1: Vanilla BBRv3',
    2: 'B3: cwnd-freeze',
    3: 'B4: pause/resume',
    4: 'BBR-SAT (ours)',
}

BASELINE_STYLES = {
    0: dict(color='#d62728', linestyle='--',  marker='s', linewidth=1.5),
    2: dict(color='#ff7f0e', linestyle='-.',  marker='^', linewidth=1.5),
    3: dict(color='#2ca02c', linestyle=':',   marker='D', linewidth=1.5),
    4: dict(color='#1f77b4', linestyle='-',   marker='o', linewidth=2.0),
}

def load_summary(path):
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append({k: float(v) if v else 0 for k, v in r.items()})
    return rows

def plot_lead_time_curve(summary_path, output_dir, orbit_from=0, orbit_to=2,
                          handover_time_s=30):
    """Plot T90 vs lead_time for LEO->GEO at T=30s (the primary result)."""
    rows = load_summary(summary_path)

    # Filter to target condition
    filtered = [r for r in rows
                if int(r['orbit_from']) == orbit_from
                and int(r['orbit_to']) == orbit_to
                and int(r['handover_time_s']) == handover_time_s
                and int(r['loss']) == 0]

    if not filtered:
        print(f"No data for orbit_from={orbit_from} orbit_to={orbit_to} "
              f"handover_time_s={handover_time_s}")
        return

    fig, axes = plt.subplots(1, 2, figsize=(10, 4))

    # --- Panel A: T90 vs lead_time ---
    ax = axes[0]
    for b in [0, 2, 3, 4]:
        brows = sorted([r for r in filtered if int(r['baseline']) == b],
                       key=lambda x: x['lead_time_s'])
        if not brows:
            continue
        lead_times = [r['lead_time_s'] for r in brows]
        t90_s = [r['t90_median_us'] / 1e6 if r['n_converged'] > 0 else None
                 for r in brows]

        # Replace None with a large sentinel for plotting
        t90_plot = [v if v is not None else 60.0 for v in t90_s]
        converged = [v is not None for v in t90_s]

        style = BASELINE_STYLES[b]
        line, = ax.plot(lead_times, t90_plot,
                        label=BASELINE_NAMES[b], **style)

        # Mark non-converged points with open markers
        for lt, t, c in zip(lead_times, t90_plot, converged):
            if not c:
                ax.plot(lt, t, marker=style['marker'],
                        color=style['color'],
                        markerfacecolor='white', markersize=8)

    ax.set_xlabel('Lead time (s)', fontsize=11)
    ax.set_ylabel('T90 (s)', fontsize=11)
    ax.set_title('(a) Convergence time vs. advance notice', fontsize=11)
    ax.legend(fontsize=9, loc='upper right')
    ax.set_ylim(bottom=0)
    ax.axhline(y=60, color='gray', linestyle=':', linewidth=0.8,
               label='_Never converged')
    ax.text(max(lead_times) * 0.95, 61, 'Never\nconverged',
            ha='right', va='bottom', fontsize=8, color='gray')
    ax.grid(True, alpha=0.3)

    # --- Panel B: Goodput vs lead_time ---
    ax = axes[1]
    for b in [0, 2, 3, 4]:
        brows = sorted([r for r in filtered if int(r['baseline']) == b],
                       key=lambda x: x['lead_time_s'])
        if not brows:
            continue
        lead_times = [r['lead_time_s'] for r in brows]
        goodput_mb = [r['goodput_mean_bytes'] / 1e6 for r in brows]

        style = BASELINE_STYLES[b]
        ax.plot(lead_times, goodput_mb,
                label=BASELINE_NAMES[b], **style)

    ax.set_xlabel('Lead time (s)', fontsize=11)
    ax.set_ylabel('Goodput (MB, 55s post-handover)', fontsize=11)
    ax.set_title('(b) Throughput recovery vs. advance notice', fontsize=11)
    ax.legend(fontsize=9, loc='lower right')
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)

    fig.suptitle(
        f'BBR-SAT vs. baselines: LEO→GEO handover at T={handover_time_s}s\n'
        f'picoquic simulation, GEO link: 10 Mbps / 580ms RTT',
        fontsize=11, y=1.02)

    plt.tight_layout()

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / 'fig3_t90_vs_lead_time.pdf'
    png_path = output_dir / 'fig3_t90_vs_lead_time.png'
    plt.savefig(pdf_path, bbox_inches='tight', dpi=150)
    plt.savefig(png_path, bbox_inches='tight', dpi=150)
    print(f"Saved: {pdf_path}")
    print(f"Saved: {png_path}")
    plt.close()


def print_table(summary_path, orbit_from=0, orbit_to=2, handover_time_s=30):
    """Print a results table for quick review."""
    rows = load_summary(summary_path)
    filtered = sorted([r for r in rows
                       if int(r['orbit_from']) == orbit_from
                       and int(r['orbit_to']) == orbit_to
                       and int(r['handover_time_s']) == handover_time_s
                       and int(r['loss']) == 0],
                      key=lambda x: (x['baseline'], x['lead_time_s']))

    print(f"\nLEO→GEO, T={handover_time_s}s, 0% loss")
    print(f"{'Baseline':>8} {'Lead(s)':>8} {'Conv':>6} "
          f"{'T90(s)':>8} {'Goodput(MB)':>12}")
    print("-" * 50)
    for r in filtered:
        t90 = f"{r['t90_median_us']/1e6:.2f}" if r['n_converged'] > 0 else "never"
        print(f"{int(r['baseline']):>8} {r['lead_time_s']:>8.0f} "
              f"{int(r['n_converged']):>6} {t90:>8} "
              f"{r['goodput_mean_bytes']/1e6:>12.2f}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 plot_lead_time_curve.py <summary_csv> [output_dir]")
        sys.exit(1)

    summary_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else str(Path(summary_path).parent)

    print_table(summary_path)
    plot_lead_time_curve(summary_path, output_dir)
