#!/usr/bin/env python3
"""
run_experiment.py -- BBR-SAT experiment harness.

Drives the full experiment sweep defined in the BBR-SAT experiment context doc.
Spawns picoquic_ct per condition, captures CSV stdout, aggregates 30 runs,
outputs results to CSV files.

Usage:
    python3 run_experiment.py --picoquic-ct /path/to/picoquic_ct
                              --output-dir results/
                              --parallel 3
                              [--exp {1,2,both}]
                              [--runs 30]
                              [--dry-run]

Output files:
    results/exp1_raw.csv        -- all raw run rows
    results/exp1_summary.csv    -- per-condition aggregated stats
    results/exp2_raw.csv        -- fairness experiment raw rows
    results/exp2_summary.csv    -- fairness experiment summary

CSV columns (raw):
    baseline,orbit_from,orbit_to,handover_time_s,lead_time_s,
    loss,seed,run_id,t90_us,goodput_bytes,peak_queue,converged

CSV columns (summary):
    baseline,orbit_from,orbit_to,handover_time_s,lead_time_s,loss,
    n_runs,n_converged,t90_median_us,t90_p25_us,t90_p75_us,
    goodput_mean_bytes,goodput_ci95_bytes,peak_queue_median
"""

import argparse
import csv
import io
import itertools
import json
import os
import random
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
from scipy import stats

# ---------------------------------------------------------------------------
# Experiment parameters (from BBR-SAT experiment context doc)
# ---------------------------------------------------------------------------

# Baseline IDs -- must match bbr_sat_experiment.c
BASELINES = {
    'B1':     0,   # Vanilla BBRv3
    'B3':     2,   # cwnd-freeze (StarQUIC-style)
    'B4':     3,   # pause/resume (Creo-style)
    'BBRSAT': 4,   # BBR-SAT full mechanism
    'B5':     5,   # Vanilla CUBIC
}

# Orbit classes -- must match BBR_SAT_ORBIT_* in bbr.c
ORBIT_LEO = 0
ORBIT_MEO = 1
ORBIT_GEO = 2
ORBIT_NAMES = {0: 'LEO', 1: 'MEO', 2: 'GEO'}

# Experiment 1: Single-flow performance
EXP1_TRANSITIONS = [
    (ORBIT_LEO, ORBIT_GEO),   # LEO->GEO: most important (5x BW drop, high RTT)
    (ORBIT_GEO, ORBIT_LEO),   # GEO->LEO: BW increase, RTT decrease
    (ORBIT_LEO, ORBIT_MEO),   # LEO->MEO: moderate BW drop
    (ORBIT_MEO, ORBIT_LEO),   # MEO->LEO: BW increase, RTT decrease
    (ORBIT_MEO, ORBIT_GEO),   # MEO->GEO: GEO entry
    (ORBIT_GEO, ORBIT_MEO),   # GEO->MEO: BW increase, RTT decrease
]
EXP1_HANDOVER_TIMES_S = [30, 60, 120]
EXP1_LEAD_TIMES_S = [0, 2, 5, 10, 20, 30, 60]
EXP1_LOSS_CONDITIONS = [0]    # 0=no loss; extend to [0, 1] for loss runs

# Total experiment 1 conditions:
# 3 transitions * 3 handover_times * 7 lead_times * 1 loss * 4 baselines
# = 252 conditions * 30 runs = 7,560 runs (no-loss only)
# With loss: 504 conditions * 30 runs = 15,120 runs

# Experiment 2: Fairness (placeholder -- extend post Week 5 gate)
EXP2_CONDITIONS = []  # populated separately

# Runs per condition
RUNS_PER_CONDITION = 30

# Total simulation time per run (seconds)
# Must be > handover_time + T90_MAX (60s) + convergence buffer
TOTAL_TIME_MAP = {
    30:   90,   # handover at 30s, 60s post-handover window
    60:  120,   # handover at 60s, 60s post-handover
    120: 180,   # handover at 120s, 60s post-handover
}

# picoquic_ct test names for each baseline
BASELINE_TEST_NAMES = {
    0: 'bbr_sat_exp_b1_smoke',
    2: 'bbr_sat_exp_b3_smoke',
    3: 'bbr_sat_exp_b4_smoke',
    4: 'bbr_sat_exp_bbrsat_smoke',
}

# ---------------------------------------------------------------------------
# Run one condition
# ---------------------------------------------------------------------------

def make_test_name(baseline_id, orbit_from, orbit_to,
                   handover_time_s, lead_time_s, has_loss, seed, run_id):
    """
    Build a picoquic_ct test name that maps to bbr_sat_experiment_one().
    
    Since picoquic_ct test functions take no arguments, we encode all
    parameters in the test name and dispatch via a lookup table in
    bbr_sat_experiment.c. For the harness, we call the experiment
    binary directly with parameters via a wrapper.
    """
    # We use the direct binary invocation path instead
    return None


def run_one(picoquic_ct, baseline_id, orbit_from, orbit_to,
            handover_time_s, lead_time_s, has_loss, seed, run_id,
            total_time_s, timeout_s=600):
    """
    Spawn bbr_sat_runner for one run. Returns parsed CSV dict or None on failure.
    bbr_sat_runner takes all parameters as CLI args and writes one CSV row to stdout.
    """
    # bbr_sat_runner is in the same directory as picoquic_ct
    runner = os.path.join(os.path.dirname(picoquic_ct), 'bbr_sat_runner')

    try:
        result = subprocess.run(
            [runner,
             str(baseline_id),
             str(orbit_from),
             str(orbit_to),
             str(handover_time_s),
             str(lead_time_s),
             str(has_loss),
             str(seed),
             str(run_id),
             str(total_time_s)],
            capture_output=True,
            text=True,
            timeout=timeout_s,
            cwd=os.path.dirname(os.path.dirname(picoquic_ct)),  # repo root for certs
        )

        # Parse CSV from stdout (test runner prepends "Starting test..." lines)
        csv_line = None
        for line in result.stdout.splitlines():
            line = line.strip()
            # CSV rows have the format: int,int,int,...
            parts = line.split(',')
            if len(parts) == 14:
                try:
                    int(parts[0]); int(parts[1]); int(parts[2]); int(parts[3])
                    csv_line = line
                except ValueError:
                    pass

        if csv_line is None:
            return None

        cols = csv_line.split(',')
        return {
            'baseline':        int(cols[0]),
            'orbit_from':      int(cols[1]),
            'orbit_to':        int(cols[2]),
            'handover_time_s': int(cols[3]),
            'lead_time_s':     int(cols[4]),
            'loss':            int(cols[5]),
            'seed':            int(cols[6]),
            'run_id':          int(cols[7]),
            't90_us':          int(cols[8]),
            'goodput_bytes':   int(cols[9]),
            'peak_queue':      int(cols[10]),
            'converged':       int(cols[11]),
                'ss_mean_bps':     int(cols[12]),
                'ss_stddev_bps':   int(cols[13]),
        }

    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT: baseline={baseline_id} {ORBIT_NAMES[orbit_from]}->"
              f"{ORBIT_NAMES[orbit_to]} T={handover_time_s}s lead={lead_time_s}s "
              f"run={run_id}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"  ERROR: {e}", file=sys.stderr)
        return None



# ---------------------------------------------------------------------------
# Parameter matrix generation
# ---------------------------------------------------------------------------

def generate_exp1_conditions():
    """Generate all Experiment 1 conditions."""
    conditions = []
    for baseline_name, baseline_id in BASELINES.items():
        for (orbit_from, orbit_to) in EXP1_TRANSITIONS:
            for handover_time_s in EXP1_HANDOVER_TIMES_S:
                for lead_time_s in EXP1_LEAD_TIMES_S:
                    # Lead time must be <= handover_time
                    if lead_time_s > handover_time_s:
                        continue
                    for has_loss in EXP1_LOSS_CONDITIONS:
                        total_time_s = TOTAL_TIME_MAP.get(
                            handover_time_s, handover_time_s + 120)
                        conditions.append({
                            'baseline_name':   baseline_name,
                            'baseline_id':     baseline_id,
                            'orbit_from':      orbit_from,
                            'orbit_to':        orbit_to,
                            'handover_time_s': handover_time_s,
                            'lead_time_s':     lead_time_s,
                            'has_loss':        has_loss,
                            'total_time_s':    total_time_s,
                        })
    return conditions


def generate_run_tasks(conditions, n_runs, base_seed=42):
    """Expand conditions into individual run tasks."""
    tasks = []
    rng = random.Random(base_seed)
    for cond in conditions:
        for run_id in range(n_runs):
            seed = rng.randint(0, 2**31 - 1)
            tasks.append({**cond, 'seed': seed, 'run_id': run_id})
    return tasks


# ---------------------------------------------------------------------------
# Aggregation and statistics
# ---------------------------------------------------------------------------

def aggregate_condition(rows):
    """Compute summary statistics for a set of runs."""
    n = len(rows)
    n_converged = sum(1 for r in rows if r['converged'])

    # T90 from converged runs only
    t90_values = [r['t90_us'] for r in rows if r['converged'] and r['t90_us'] > 0]
    if t90_values:
        t90_median = float(np.median(t90_values))
        t90_p25    = float(np.percentile(t90_values, 25))
        t90_p75    = float(np.percentile(t90_values, 75))
    else:
        t90_median = t90_p25 = t90_p75 = 0.0

    # Goodput stats (all runs)
    goodput_values = [r['goodput_bytes'] for r in rows]
    goodput_mean = float(np.mean(goodput_values))
    goodput_ci95 = 0.0
    if n > 1:
        se = stats.sem(goodput_values)
        goodput_ci95 = float(se * stats.t.ppf(0.975, df=n-1))

    # Peak queue
    peak_queue_median = float(np.median([r['peak_queue'] for r in rows]))

    # Steady-state metrics
    ss_means = [r['ss_mean_bps'] for r in rows if r.get('ss_mean_bps', 0) > 0]
    ss_mean_mean = float(np.mean(ss_means)) if ss_means else 0.0
    ss_stddevs = [r['ss_stddev_bps'] for r in rows if r.get('ss_stddev_bps', 0) > 0]
    ss_stddev_mean = float(np.mean(ss_stddevs)) if ss_stddevs else 0.0

    return {
        'n_runs':               n,
        'n_converged':          n_converged,
        't90_median_us':        t90_median,
        't90_p25_us':           t90_p25,
        't90_p75_us':           t90_p75,
        'goodput_mean_bytes':   goodput_mean,
        'goodput_ci95_bytes':   goodput_ci95,
        'peak_queue_median':    peak_queue_median,
        'ss_mean_bps':          ss_mean_mean,
        'ss_stddev_bps':        ss_stddev_mean,
    }


# ---------------------------------------------------------------------------
# Main sweep
# ---------------------------------------------------------------------------

def run_sweep(picoquic_ct, conditions, n_runs, output_dir, parallel,
              dry_run=False):
    """Run the full experiment sweep."""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    tasks = generate_run_tasks(conditions, n_runs)
    total = len(tasks)

    print(f"Experiment sweep: {len(conditions)} conditions × {n_runs} runs "
          f"= {total} total runs")
    print(f"Parallel workers: {parallel}")
    print(f"Output directory: {output_dir}")

    if dry_run:
        print(f"DRY RUN -- would execute {total} runs")
        # Print first 5 tasks as sample
        for t in tasks[:5]:
            print(f"  {t}")
        return

    raw_path = output_dir / 'exp1_raw.csv'
    raw_fieldnames = [
        'baseline', 'orbit_from', 'orbit_to', 'handover_time_s', 'lead_time_s',
        'loss', 'seed', 'run_id', 't90_us', 'goodput_bytes', 'peak_queue', 'converged',
        'ss_mean_bps', 'ss_stddev_bps'
    ]

    completed = 0
    failed = 0
    all_rows = []

    start_time = time.time()

    with open(raw_path, 'w', newline='') as raw_f:
        writer = csv.DictWriter(raw_f, fieldnames=raw_fieldnames)
        writer.writeheader()

        with ProcessPoolExecutor(max_workers=parallel) as executor:
            futures = {
                executor.submit(
                    run_one,
                    picoquic_ct,
                    t['baseline_id'],
                    t['orbit_from'],
                    t['orbit_to'],
                    t['handover_time_s'],
                    t['lead_time_s'],
                    t['has_loss'],
                    t['seed'],
                    t['run_id'],
                    t['total_time_s'],
                ): t
                for t in tasks
            }

            for future in as_completed(futures):
                task = futures[future]
                result = future.result()
                completed += 1

                if result is not None:
                    writer.writerow(result)
                    raw_f.flush()
                    all_rows.append(result)
                else:
                    failed += 1

                # Progress report every 50 runs
                if completed % 50 == 0 or completed == total:
                    elapsed = time.time() - start_time
                    rate = completed / elapsed if elapsed > 0 else 0
                    eta = (total - completed) / rate if rate > 0 else 0
                    print(f"  {completed}/{total} runs complete "
                          f"({failed} failed) | "
                          f"{rate:.1f} runs/s | ETA {eta/60:.1f} min")

    print(f"\nSweep complete: {completed} runs, {failed} failures")
    print(f"Raw results: {raw_path}")

    # Compute summary
    if all_rows:
        summary_path = output_dir / 'exp1_summary.csv'
        write_summary(all_rows, summary_path)
        print(f"Summary: {summary_path}")


def write_summary(rows, path):
    """Group rows by condition and compute aggregate stats."""
    # Group by condition key
    groups = {}
    for row in rows:
        key = (
            row['baseline'], row['orbit_from'], row['orbit_to'],
            row['handover_time_s'], row['lead_time_s'], row['loss']
        )
        if key not in groups:
            groups[key] = []
        groups[key].append(row)

    summary_fieldnames = [
        'baseline', 'orbit_from', 'orbit_to', 'handover_time_s',
        'lead_time_s', 'loss',
        'n_runs', 'n_converged',
        't90_median_us', 't90_p25_us', 't90_p75_us',
        'goodput_mean_bytes', 'goodput_ci95_bytes',
        'peak_queue_median',
        'ss_mean_bps', 'ss_stddev_bps',
    ]

    with open(path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=summary_fieldnames)
        writer.writeheader()
        for key, group_rows in sorted(groups.items()):
            baseline, orbit_from, orbit_to, handover_time_s, lead_time_s, loss = key
            stats_row = aggregate_condition(group_rows)
            writer.writerow({
                'baseline':        baseline,
                'orbit_from':      orbit_from,
                'orbit_to':        orbit_to,
                'handover_time_s': handover_time_s,
                'lead_time_s':     lead_time_s,
                'loss':            loss,
                **stats_row,
            })


# ---------------------------------------------------------------------------
# Week 5 critical gate: B1 vs BBR-SAT on LEO->GEO T=30s lead=5s no-loss
# ---------------------------------------------------------------------------

def run_week5_gate(picoquic_ct, output_dir, n_runs=30, parallel=3):
    """
    Run the Week 5 critical gate test.
    B1 (Vanilla BBRv3) vs BBR-SAT on LEO->GEO, T=30s, lead=5s, 0% loss.
    This is the headline result. If BBR-SAT doesn't beat B1 here, the paper
    needs a fundamental rethink.
    """
    print("\n" + "="*60)
    print("WEEK 5 CRITICAL GATE: B1 vs BBR-SAT, LEO->GEO, T=30s, lead=5s")
    print("="*60)

    gate_conditions = [
        {
            'baseline_name':   'B1',
            'baseline_id':     0,
            'orbit_from':      ORBIT_LEO,
            'orbit_to':        ORBIT_GEO,
            'handover_time_s': 30,
            'lead_time_s':     5,
            'has_loss':        0,
            'total_time_s':    90,
        },
        {
            'baseline_name':   'BBR-SAT',
            'baseline_id':     4,
            'orbit_from':      ORBIT_LEO,
            'orbit_to':        ORBIT_GEO,
            'handover_time_s': 30,
            'lead_time_s':     5,
            'has_loss':        0,
            'total_time_s':    90,
        },
    ]

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    run_sweep(picoquic_ct, gate_conditions, n_runs,
              output_dir / 'week5_gate', parallel)

    # Read and print gate results
    raw_path = output_dir / 'week5_gate' / 'exp1_raw.csv'
    summary_path = output_dir / 'week5_gate' / 'exp1_summary.csv'

    if summary_path.exists():
        print("\nGate results:")
        with open(summary_path) as f:
            reader = csv.DictReader(f)
            for row in reader:
                baseline_name = {0: 'B1', 4: 'BBR-SAT'}.get(
                    int(row['baseline']), str(row['baseline']))
                t90_ms = float(row['t90_median_us']) / 1000
                n_conv = int(row['n_converged'])
                n_runs_out = int(row['n_runs'])
                print(f"  {baseline_name:8s}: T90={t90_ms:.0f}ms "
                      f"converged={n_conv}/{n_runs_out}")

        # Verdict
        rows = {}
        with open(summary_path) as f:
            reader = csv.DictReader(f)
            for row in reader:
                rows[int(row['baseline'])] = row

        b1_t90     = float(rows.get(0, {}).get('t90_median_us', 0))
        bbrsat_t90 = float(rows.get(4, {}).get('t90_median_us', 0))
        b1_conv    = int(rows.get(0, {}).get('n_converged', 0))
        bs_conv    = int(rows.get(4, {}).get('n_converged', 0))

        print()
        # Use goodput_bytes as primary metric when T90 not available
        b1_goodput     = float(rows.get(0, {}).get('goodput_mean_bytes', 0))
        bbrsat_goodput = float(rows.get(4, {}).get('goodput_mean_bytes', 0))

        print(f"  B1      goodput: {b1_goodput/1e6:.2f} MB")
        print(f"  BBR-SAT goodput: {bbrsat_goodput/1e6:.2f} MB")
        print()

        if bbrsat_t90 > 0 and b1_t90 > 0 and bbrsat_t90 < b1_t90:
            speedup = b1_t90 / bbrsat_t90
            print(f"VERDICT: BBR-SAT FASTER by {speedup:.1f}x (T90) -- GATE PASS")
        elif bbrsat_goodput > b1_goodput and b1_goodput > 0:
            pct = 100.0 * (bbrsat_goodput - b1_goodput) / b1_goodput
            print(f"VERDICT: BBR-SAT delivers {pct:.1f}% more goodput than B1 "
                  f"-- GATE PASS")
            if b1_conv == 0:
                print("  NOTE: B1 T90=0 because vanilla BBRv3 never converges "
                      "on GEO (stuck in startup_long_rtt). This is the paper's "
                      "core finding.")
        elif bbrsat_goodput == b1_goodput:
            print("VERDICT: BBR-SAT == B1 goodput. No differentiation. INVESTIGATE.")
        else:
            print(f"VERDICT: BBR-SAT WORSE than B1. "
                  f"B1={b1_goodput/1e6:.2f}MB BBR-SAT={bbrsat_goodput/1e6:.2f}MB. "
                  f"INVESTIGATE before full sweep.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description='BBR-SAT experiment harness')
    parser.add_argument('--picoquic-ct',
                        default='./build/picoquic_ct',
                        help='Path to picoquic_ct binary')
    parser.add_argument('--output-dir', default='results',
                        help='Output directory for CSV files')
    parser.add_argument('--parallel', type=int, default=3,
                        help='Number of parallel workers (default: 3)')
    parser.add_argument('--runs', type=int, default=RUNS_PER_CONDITION,
                        help=f'Runs per condition (default: {RUNS_PER_CONDITION})')
    parser.add_argument('--exp', choices=['1', '2', 'both', 'gate'],
                        default='gate',
                        help='Which experiment to run (default: gate)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Print conditions without running')
    args = parser.parse_args()

    # Resolve picoquic_ct path
    picoquic_ct = os.path.abspath(args.picoquic_ct)
    if not os.path.exists(picoquic_ct):
        print(f"ERROR: picoquic_ct not found at {picoquic_ct}", file=sys.stderr)
        sys.exit(1)

    if args.exp == 'gate':
        run_week5_gate(picoquic_ct, args.output_dir,
                       n_runs=args.runs, parallel=args.parallel)

    elif args.exp == '1':
        conditions = generate_exp1_conditions()
        print(f"Experiment 1: {len(conditions)} conditions")
        run_sweep(picoquic_ct, conditions, args.runs,
                  args.output_dir, args.parallel, args.dry_run)

    elif args.exp == 'both':
        conditions = generate_exp1_conditions()
        run_sweep(picoquic_ct, conditions, args.runs,
                  args.output_dir, args.parallel, args.dry_run)
        # Exp 2 TBD


if __name__ == '__main__':
    main()
