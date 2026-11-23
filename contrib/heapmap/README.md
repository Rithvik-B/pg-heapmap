# heapmap: a learned block-range index for PostgreSQL 18

```sql
CREATE EXTENSION heapmap;
CREATE INDEX events_ts ON events USING heapmap (ts);
```

For a column whose values follow the physical order of the table (a timestamp in
a log, a serial id), "key to heap page" is a smooth increasing function. This
index stores a model of that function and **nothing per row**. A lookup asks the
model which few heap pages to read, and PostgreSQL filters the rows on them.

It targets the gap between PostgreSQL's two existing choices for such columns: a
B-tree is exact but as large as the column, and BRIN is tiny but reads a whole
block range (128 pages by default) for every lookup.

Status: working prototype. It passes the correctness suite below. See "Known
limits" for what is not done.

## How it works

Three parts, and one rule that ties them together.

* **Model.** A piecewise-linear function from key to heap page. Each segment has
  an error window, usually 1 to 3 pages. Segments are sealed once written and
  never change, except the newest one, which stays open (see below).
* **Tail.** The heap pages at the end of the table that are still being filled.
  They are not in the model yet and every lookup of a recent key reads all of them.
* **Exceptions.** Rows that are not where the model says: late arrivals, rows an
  UPDATE moved, bad timestamps. These are stored exactly, as (key, row pointer),
  in a small B+-tree inside the index.

The rule: every indexed row is in the tail with a key at or above the tail's
fence, or on a page inside the model's window for its key, or in the exception
tree. `CREATE INDEX` establishes this, every insert re-checks it for the new row,
and folding re-checks it for each row it moves from the tail into the model.

**Inserts.** A row that arrives in order lands in the tail and the index writes
nothing at all. A row that is out of place costs one exception entry.

**Folding.** Each time the table has grown by a few pages, the inserting backend
reads the tail, extends the model over the pages that are settled, and moves the
frontier. VACUUM does the same.

**Fitting, robust to messy tables.** Each heap page is summarised by its median
key, the longest non-decreasing run of those medians is kept, and segments are
fitted through that run. A batch of rows loaded out of place drops out as a
whole and its rows become exceptions. How tightly the segments follow the data
is chosen from the data. A key value that fills many pages gets a flat segment
whose window is exactly the pages holding it.

If the column does not follow physical order at all, every row becomes an
exception and the index is, in effect, a B-tree.

### Concurrent writers

When several sessions insert at once, each fills a table page of its own.
PostgreSQL also grows a busy table up to 64 pages at a time and hands those
pages out in no particular order. So neighbouring keys end up many pages apart,
and a page that looks abandoned may simply not have had its turn. Three things
deal with that:

* **The frontier only passes settled pages**: full ones, and ones that gained no
  row while plenty of rows arrived elsewhere. A model fitted to half-filled pages
  describes rows that are not all there yet.
* **The newest segment stays open.** Rows for keys it already answers for keep
  turning up on later pages, and its window grows to take them in, as long as
  that costs lookups less than storing those rows exactly. When new rows stop
  fitting its line, it is sealed and new segments are fitted.
* **Stray rows cannot steer the model.** A window does not stretch for one or
  two rows, a new segment needs a real share of the rows to stand on, sparsely
  filled pages are ignored when fitting, and the tail's fence comes back down if
  a row with a key from the future pushed it up.

The result is that lookup cost degrades gradually with the number of concurrent
writers (the window widens to the scatter that is physically there) while the
index stays small. Numbers are under "Results".

## Files

| file | contents |
|---|---|
| `heapmap.h` | page layouts, the state page, shared declarations |
| `hm_model.c` | evaluating the model, fitting it, deciding which rows it covers |
| `hm_build.c` | `CREATE INDEX` / `REINDEX`, appending sealed segments |
| `hm_insert.c` | inserts, folding the tail into the model, VACUUM |
| `hm_scan.c` | scans (bitmap only, like BRIN) |
| `hm_exc.c` | the exception B+-tree |
| `hm_am.c` | access method handler, options, segment cache, inspection functions |
| `test/` | correctness suite |
| `demo/demo.sh` | live walk-through against B-tree and BRIN; `demo/DEMO_GUIDE.md` says what to show and say |
| `demo/demo_real.sh` | second act on one day of real GitHub events |
| `eval/quick_compare.sh` | 10M-row comparison with btree and BRIN |
| `eval/concurrent.sh` | behaviour under 1 to 32 concurrent writers |
| `eval/gharchive.sh` | a real event stream: one day of GH Archive, with `eval/gharchive_extract.py` |
| `eval/scale.sh` | 1M to 50M rows |
| `eval/plots.py`, `eval/results/` | figures for the report, raw output of every run |
| `doc/` | the project report (`report.html`, built to `report.pdf` by `build_report.sh`) and its figures |

## Running it

This directory is `contrib/heapmap` of a PostgreSQL 18.7 source tree. It builds
like any contrib module, in-tree (`./configure` at the top, then
`make -C contrib/heapmap`) or against an installed PostgreSQL 18 with its
server headers (`postgresql-server-dev-18`) using `make USE_PGXS=1`. The
scripts below use the installed one.

A private test cluster, which needs no sudo and leaves the system one alone:

```
./cluster.sh init        # private cluster on port 5545, first time only
./cluster.sh install     # build and install the extension
./cluster.sh psql
demo/demo.sh             # live walk-through, about 3 minutes
demo/demo_real.sh        # one day of real GitHub events (after eval/gharchive.sh)
test/run_tests.sh        # correctness suite, about 4 minutes
eval/quick_compare.sh    # 10M rows against btree and BRIN, about 6 minutes
eval/concurrent.sh       # 1 to 32 concurrent writers
eval/gharchive.sh        # real data, needs the CSV described at the top of the script
eval/scale.sh            # 1M to 50M rows, about 15 minutes
```

Or in the system PostgreSQL, so that it shows up in pgAdmin too:

```
make USE_PGXS=1 && sudo make USE_PGXS=1 install
export PGHOST=localhost PGUSER=postgres PGPASSWORD=... PGDATABASE=yourdb   # as in pgAdmin
HM_SYSTEM=1 demo/demo.sh
```

The demo creates the extension in that database and six tables whose names
start with `ev_`, and leaves them there to query afterwards.

`demo/demo.sh [rows] [clients]` builds the same event table three times, one
index each, and measures everything live: build time, sizes, pages read per
lookup, a range query, loading more rows, late rows, concurrent inserts with the
index sizes sampled every two seconds, and a check that the index and a full
table scan return the same rows. `AUTO=1` runs it without pauses.
`demo/sample_output.txt` is one run of it.

Useful while exploring:

```sql
SELECT * FROM heapmap_stats('events_ts');        -- segments, windows, tail, exceptions
SELECT * FROM heapmap_segments('events_ts');     -- the model itself, one row per segment
SELECT * FROM heapmap_window('events_ts', 123);  -- which heap pages a lookup of key 123 reads
```

Options: `epsilon` (how tightly segments follow the data, 0 = automatic),
`exception_pct` (a page holding less than this percent of a segment's rows is
cut from its window and its rows stored as exceptions, default 0.25: lower means
fewer exceptions and wider windows), `max_window` (rows further than this many
pages from the model are always exceptions, default 64), `fold_after` (default
16), `keep` (pages always left in the tail, default 4) and `max_tail` (beyond
this many pages the tail is folded even if still filling, default 512).

Key types: `bigint`, `integer`, `timestamp`, `timestamptz`, `date`.

## Testing

`test/run_tests.sh` compares the exact set of rows the index returns with a
sequential scan, for random equality and range predicates, on sixteen tables:
sorted, jittered, 2% late rows, whole batches out of place, heavy duplicates,
fully random, negative keys, NULLs, empty, one row, one repeated key, rows of
very different lengths, and each key type. The checks are repeated after
appends, late inserts, row-at-a-time inserts, updates that move rows and updates
that do not, deletes, VACUUM, reuse of freed space, and REINDEX. It then runs 16
concurrent clients inserting, reading and deleting, and finally stops the server
with an immediate shutdown right after a bulk insert and checks again after
recovery.

## Results

PostgreSQL 18.7, i7-13620H (16 threads), everything cached. One run on one
machine: treat the timings as indicative and the sizes and page counts as exact.
Raw output is in `eval/results/`.

### One writer: 10M rows, then 2M more

Keys increasing with a random gap, `(k bigint, payload int)`, 423 MB table.
Timings are medians of 5 rounds with the indexes taking turns.

| | btree | BRIN | BRIN, 8 pages per range | heapmap |
|---|---|---|---|---|
| Index size | 214 MB | 32 kB | 240 kB | 24 kB |
| Build | 2.18 s | 0.78 s | 0.79 s | 0.86 s |
| Pages read per point lookup | 4 | 131 | 45 | 5 |
| Point lookup, in server | 2.6 µs | 537 µs | 567 µs | 15.9 µs |
| Range query, about 10,000 rows | 735 µs | 1,250 µs | 1,247 µs | 594 µs |
| Loading 2M more rows | 2.24 s | 1.02 s | 0.94 s | 1.23 s |
| WAL written by that load | 264 MB | 139 MB | 138 MB | 138 MB |
| Index size after the load | 257 MB | 32 kB | 240 kB | 24 kB |

Read plainly: the index is the size of BRIN and about 30 times faster than BRIN
on point lookups. Against the B-tree it is several thousand times smaller, loads
data about twice as fast and is on par for range queries, but a cached point
lookup is about six times slower, because it filters whole heap pages where the
B-tree goes straight to the row.

### Many writers

Each session runs `INSERT INTO t VALUES (nextval('seq'))` for 8 seconds,
`synchronous_commit` off. "Window" is the average number of heap pages a lookup
reads; it grows because that is how far apart PostgreSQL puts neighbouring keys.

| Sessions | Rows | Stored as exceptions | Window | heapmap | btree |
|---|---|---|---|---|---|
| 1 | 0.40M | 0.00% | 2 | 24 kB | 8.8 MB |
| 2 | 0.69M | 0.03% | 4.5 | 24 kB | 15 MB |
| 4 | 1.13M | 0.04% | 9 | 40 kB | 24 MB |
| 8 | 1.81M | 0.07% | 21 | 96 kB | 39 MB |
| 16 | 2.18M | 1.15% | 36 | 752 kB | 47 MB |
| 32 | 2.17M | 6.20% | 47 | 2.9 MB | 47 MB |

With a mix of single-row inserts, 300-row inserts, late rows, reads and deletes:
0.25% exceptions with 4 sessions (384 kB against a 92 MB btree) and 1.66% with
16 (1.3 MB against 58 MB).

Before the open segment and the settled-page rule, the 16-session run stored 86%
of its rows as exceptions and the index was as large as the B-tree.

### A real event stream: one day of GitHub

Every public GitHub event of 1 January 2024 from GH Archive, 3,879,843 events
in the order they were recorded, indexed on `created_at` (one-second
resolution, 45 events per second). 0.19% of events have a timestamp behind an
earlier event, by up to 58 minutes. The first 95% are loaded and indexed, the
last 5% appended afterwards. Raw output: `eval/results/gharchive_1day.txt`.

| | btree | BRIN | heapmap |
|---|---|---|---|
| Index size (table 342 MB) | 25 MB | 32 kB | 136 kB |
| Build time | 0.91 s | 0.35 s | 0.74 s |
| Pages read per "events at this second" (45 rows) | 4.0 | 459.6 | 8.1 |
| Time per such lookup, cached | 11 µs | 1,867 µs | 29 µs |
| "Everything in a 5-minute window" (13,500 rows) | 480 µs | 3,142 µs | 827 µs |
| Appending the last 194k events | 0.45 s, 46 MB WAL | 0.37 s, 34 MB | 0.40 s, 34 MB |
| Index size after the append | 27 MB | 32 kB | 176 kB |

The model has 261 segments with an average window of 5.3 pages and 6,390
exception entries, 0.17% of the rows: the share of the stream that really is
out of order. BRIN suffers from exactly those rows, because one late event in
a 128-page range stretches that range's minimum and a lookup then matches
several ranges.

### Scaling with table size

Same generated rows as the 10M comparison. Raw output: `eval/results/scale.txt`.

| Rows | btree size | heapmap size | btree build | heapmap build | pages per lookup btree / BRIN / heapmap | heapmap model |
|---|---|---|---|---|---|---|
| 1M | 21 MB | 24 kB | 0.21 s | 0.14 s | 4 / 130 / 5 | 3 segments, 13 exceptions |
| 5M | 107 MB | 24 kB | 0.91 s | 0.46 s | 4 / 130 / 5 | 19 segments, 96 exceptions |
| 20M | 428 MB | 24 kB | 3.95 s | 1.57 s | 4 / 132 / 5 | 71 segments, 361 exceptions |
| 50M | 1,071 MB | 40 kB | 8.76 s | 3.87 s | 5 / 140 / 6 | 179 segments, 935 exceptions |

## Known limits

* **Very many writers.** The window grows with the number of sessions inserting
  at once, and past `max_window` pages of scatter rows become exceptions (6% at
  32 sessions on this machine). 64 sessions on 16 threads was not usable.
* **Point lookups are slower than a B-tree when cached**, as above.
* **Bitmap scans only.** No `ORDER BY` from the index, no unique constraints, no
  index-only scans, one column.
* **Updates.** An update that moves a row to another page costs an exception.
  The index suits append-mostly tables.
* **Space reused far from the end of the table.** Rows that PostgreSQL places in
  old pages are out of place and cost an exception each. That includes space
  freed by DELETE and VACUUM, short rows that fit into gaps left in earlier
  pages (about 1% of rows in the variable-length test table), and the run of up
  to 64 empty pages a bulk load leaves behind, if it is filled only after the
  model has moved past it.
* **Sessions that pause.** A session that stops inserting for a while and then
  carries on in the page it had been filling may find the frontier has passed
  it; the rest of that page becomes exceptions.
* **Build memory.** `CREATE INDEX` holds one 16-byte record per row in memory.
* **The planner** estimates bitmap heap scans as if rows were scattered, so it
  gives up on this index (and on BRIN) for wide ranges sooner than it should.
* Not yet measured: cold-cache lookups.
