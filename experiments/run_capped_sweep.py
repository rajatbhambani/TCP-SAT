#!/usr/bin/env python3
"""
run_capped_sweep.py -- BBR-SAT 1×BDP capped-buffer sweep.

Runs the primary evaluation with a 218 KB (1×GEO BDP) droptail cap.
This is the realistic buffer model that becomes the primary paper result.

Usage:
    cd /home/rajat/tcpsatproject/picoquic-main
    python3 ../experiments/run_capped_sweep.py [--parallel N]
"""

import subprocess, csv, os, sys, time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
import numpy as np

RUNNER = Path('/home/rajat/tcpsatproject/picoquic-main/build/bbr_sat_runner')
OUT_DIR = Path('/home/rajat/tcpsatproject/results/capped_sweep_v1')
OUT_DIR.mkdir(parents=True, exist_ok=True)

# Baselines: 0=B1, 2=B3, 3=B4, 4=BBR-SAT, 5=CUBIC
BASELINES = [0, 2, 3, 4, 5]
# Orbits: 0=LEO, 1=MEO, 2=GEO
TRANSITIONS = [(0,1),(0,2),(1,0),(1,2),(2,0),(2,1)]
HANDOVER_TIMES = [30, 60, 120]
LEAD_TIMES = [0, 2, 5, 10, 20, 30]
RUNS = 10
USE_BDP_CAP = 1

TOTAL_TIME_MAP = {30: 90, 60: 120, 120: 180}

RAW_HEADER = ('baseline,orbit_from,orbit_to,handover_time_s,lead_time_s,'
              'loss,seed,run_id,t90_us,goodput_bytes,peak_queue,converged,'
              'ss_mean_bps,ss_stddev_bps')


def seed_for(run_id):
    import random; random.seed(run_id * 1_000_003 + 42)
    return random.randint(0, 2**31-1)


def run_one(args):
    bl, of, ot, ho, lead, run_id = args
    seed = seed_for(run_id)
    total = TOTAL_TIME_MAP[ho]
    cmd = [str(RUNNER), str(bl), str(of), str(ot), str(ho), str(lead),
           '0', str(seed), str(run_id), str(total), str(USE_BDP_CAP)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=300, cwd=str(RUNNER.parent))
        for line in r.stdout.splitlines():
            if line.startswith(str(bl) + ','):
                return line.strip()
    except Exception as e:
        print(f'FAIL {cmd}: {e}', file=sys.stderr)
    return None


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--parallel', type=int, default=os.cpu_count())
    args = p.parse_args()

    jobs = [(bl, of, ot, ho, lead, rid)
            for bl in BASELINES
            for (of, ot) in TRANSITIONS
            for ho in HANDOVER_TIMES
            for lead in LEAD_TIMES
            if lead <= ho
            for rid in range(RUNS)]

    print(f'Total jobs: {len(jobs)}, parallel: {args.parallel}')
    raw_path = OUT_DIR / 'exp1_raw.csv'
    with open(raw_path, 'w') as f:
        f.write(RAW_HEADER + '\n')

    done = 0
    t0 = time.time()
    with ProcessPoolExecutor(max_workers=args.parallel) as ex:
        futs = {ex.submit(run_one, j): j for j in jobs}
        with open(raw_path, 'a') as f:
            for fut in as_completed(futs):
                done += 1
                row = fut.result()
                if row:
                    f.write(row + '\n')
                if done % 100 == 0:
                    el = time.time() - t0
                    print(f'  {done}/{len(jobs)} done  ({el:.0f}s)', flush=True)

    print(f'Raw data: {raw_path}')
    summarize(raw_path)


def summarize(raw_path):
    from collections import defaultdict
    rows = list(csv.DictReader(open(raw_path)))
    by_cond = defaultdict(list)
    for r in rows:
        key = (r['baseline'], r['orbit_from'], r['orbit_to'],
               r['handover_time_s'], r['lead_time_s'], r['loss'])
        by_cond[key].append(r)

    sum_path = OUT_DIR / 'exp1_summary.csv'
    hdr = ('baseline,orbit_from,orbit_to,handover_time_s,lead_time_s,loss,'
           'n_runs,n_converged,t90_median_us,t90_p25_us,t90_p75_us,'
           'goodput_mean_bytes,goodput_ci95_bytes,peak_queue_median,'
           'ss_mean_bps,ss_stddev_bps')
    with open(sum_path, 'w') as f:
        f.write(hdr + '\n')
        for key in sorted(by_cond):
            rs = by_cond[key]
            n = len(rs)
            conv = [r for r in rs if r['converged'] == '1']
            nc = len(conv)
            t90s = [float(r['t90_us']) for r in conv]
            t90_med = np.median(t90s) if t90s else 0.0
            t90_p25 = np.percentile(t90s, 25) if t90s else 0.0
            t90_p75 = np.percentile(t90s, 75) if t90s else 0.0
            gps = [float(r['goodput_bytes']) for r in rs]
            gp_mean = np.mean(gps)
            gp_ci = 1.96 * np.std(gps) / np.sqrt(n) if n > 1 else 0.0
            pqs = [float(r['peak_queue']) for r in rs]
            pq_med = np.median(pqs)
            ss = [float(r['ss_mean_bps']) for r in rs if 'ss_mean_bps' in r]
            ss_mean = np.mean(ss) if ss else 0.0
            ss_std = np.std(ss) if ss else 0.0
            f.write(','.join([*key, str(n), str(nc),
                              f'{t90_med:.1f}', f'{t90_p25:.1f}', f'{t90_p75:.1f}',
                              f'{gp_mean:.1f}', f'{gp_ci:.1f}',
                              f'{pq_med:.1f}', f'{ss_mean:.1f}', f'{ss_std:.1f}']) + '\n')
    print(f'Summary: {sum_path}')


if __name__ == '__main__':
    main()
