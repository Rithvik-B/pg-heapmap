#!/usr/bin/env python3
"""Figures for the report, from eval/results/*.txt and eval/data/*.csv.
Writes doc/figures/*.png. Numbers that come from eval/results are copied here
so that the figures do not depend on parsing free text; the source of each is
named next to it."""
import csv, os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'doc', 'figures')
os.makedirs(OUT, exist_ok=True)
plt.rcParams.update({'font.size': 9, 'figure.dpi': 150})

# ---- 1. the model fitted to one day of real GitHub events (eval/data/gh_*.csv)
pts = [(int(p), int(k)) for p, k in csv.reader(open(os.path.join(HERE, 'data', 'gh_points.csv')))]
segs = [(int(s), int(k0), float(slope), float(p0), int(lo), int(hi))
        for s, k0, slope, p0, lo, hi in csv.reader(open(os.path.join(HERE, 'data', 'gh_segments.csv')))]
t0 = min(k for _, k in pts)
hours = lambda k: (k - t0) / 3.6e9
fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.9))
ax = axes[0]
ax.scatter([hours(k) for _, k in pts], [p for p, _ in pts], s=1, color='#888', label='rows (0.5% sample)')
for i, (s, k0, slope, p0, lo, hi) in enumerate(segs):
    k1 = segs[i + 1][1] if i + 1 < len(segs) else max(k for _, k in pts)
    ax.plot([hours(k0), hours(k1)], [p0, p0 + slope * (k1 - k0)], color='#d62728', lw=1,
            label='model segments' if i == 0 else None)
ax.set_xlabel('hours since the first event'); ax.set_ylabel('heap page'); ax.legend(loc='upper left', frameon=False)
ax.set_title('created_at to heap page, 3.9M real events', fontsize=9)
ax = axes[1]
lo_h, hi_h = 6.0, 6.05
sel = [(hours(k), p) for p, k in pts if lo_h <= hours(k) <= hi_h]
ax.scatter([h for h, _ in sel], [p for _, p in sel], s=4, color='#888')
for i, (s, k0, slope, p0, lo, hi) in enumerate(segs):
    k1 = segs[i + 1][1] if i + 1 < len(segs) else max(k for _, k in pts)
    if hours(k1) < lo_h or hours(k0) > hi_h:
        continue
    a, b = max(hours(k0), lo_h), min(hours(k1), hi_h)
    ka, kb = t0 + a * 3.6e9, t0 + b * 3.6e9
    ya, yb = p0 + slope * (ka - k0), p0 + slope * (kb - k0)
    ax.plot([a, b], [ya, yb], color='#d62728', lw=1)
    ax.fill_between([a, b], [ya + lo, yb + lo], [ya + hi + 1, yb + hi + 1], color='#d62728', alpha=0.15, lw=0)
ax.set_xlabel('hours since the first event'); ax.set_ylabel('heap page')
ax.set_title('three minutes of it, window shaded', fontsize=9)
fig.tight_layout(); fig.savefig(os.path.join(OUT, 'model_real_data.png')); plt.close(fig)

# ---- 2. scaling with table size (eval/results/scale.txt)
rows = [1e6, 5e6, 20e6, 50e6]
size = {'btree': [22487040, 112336896, 449249280, 1123115008],
        'BRIN': [24576, 24576, 40960, 90112],
        'heapmap': [24576, 24576, 24576, 40960]}
pages = {'btree': [4.0, 4.0, 4.0, 5.0], 'BRIN': [130.1, 130.1, 132.1, 140.1],
         'heapmap': [5.0, 5.0, 5.0, 6.0]}
colors = {'btree': '#1f77b4', 'BRIN': '#2ca02c', 'heapmap': '#d62728'}
fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.7))
for k in size:
    axes[0].plot(rows, [s / 1024 for s in size[k]], 'o-', color=colors[k], label=k)
    axes[1].plot(rows, pages[k], 'o-', color=colors[k], label=k)
axes[0].set_xscale('log'); axes[0].set_yscale('log'); axes[0].set_xlabel('rows'); axes[0].set_ylabel('index size (kB)')
axes[0].set_title('index size', fontsize=9); axes[0].legend(frameon=False)
axes[1].set_xscale('log'); axes[1].set_yscale('log'); axes[1].set_xlabel('rows'); axes[1].set_ylabel('pages read per point lookup')
axes[1].set_title('pages touched by one lookup', fontsize=9)
fig.tight_layout(); fig.savefig(os.path.join(OUT, 'scale.png')); plt.close(fig)

# ---- 3. concurrent writers (eval/results/concurrent.txt, 8 s runs)
sessions = [1, 2, 4, 8, 16, 32]
exc_pct = [0.00, 0.03, 0.04, 0.07, 1.15, 6.20]
window = [2, 4.5, 9, 21, 36, 47]
hm_kb = [24, 24, 40, 96, 752, 2900]
bt_kb = [8800, 15000, 24000, 39000, 47000, 47000]
fig, axes = plt.subplots(1, 3, figsize=(7.2, 2.5))
axes[0].plot(sessions, exc_pct, 'o-', color='#d62728'); axes[0].set_ylabel('% of rows stored exactly'); axes[0].set_title('exceptions', fontsize=9)
axes[1].plot(sessions, window, 'o-', color='#d62728'); axes[1].set_ylabel('heap pages per lookup'); axes[1].set_title('window', fontsize=9)
axes[2].plot(sessions, hm_kb, 'o-', color='#d62728', label='heapmap'); axes[2].plot(sessions, bt_kb, 'o-', color='#1f77b4', label='btree')
axes[2].set_yscale('log'); axes[2].set_ylabel('index size (kB)'); axes[2].set_title('index size', fontsize=9); axes[2].legend(frameon=False)
for ax in axes:
    ax.set_xscale('log', base=2); ax.set_xlabel('inserting sessions'); ax.set_xticks(sessions); ax.set_xticklabels(sessions)
fig.tight_layout(); fig.savefig(os.path.join(OUT, 'concurrent.png')); plt.close(fig)
print('wrote', OUT)
