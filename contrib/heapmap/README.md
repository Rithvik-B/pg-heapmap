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
  an error window, usually 1 to 3 pages. Segments are never changed once written.
* **Tail.** The last few heap pages, which are still being filled. They are not
  in the model yet and every lookup of a recent key reads all of them.
* **Exceptions.** Rows that are not where the model says: late arrivals, rows an
  UPDATE moved, bad timestamps. These are stored exactly, as (key, row pointer),
  in a small B+-tree inside the index.

The rule: every indexed row is in the tail with a key at or above the tail's
fence, or on a page inside the model's window for its key, or in the exception
tree. `CREATE INDEX` establishes this, every insert re-checks it for the new row,
and folding re-checks it for each row it moves from the tail into the model.

**Inserts.** A row that arrives in order lands in the tail and the index writes
nothing at all. A row that is out of place costs one exception entry.

**Folding.** When the tail grows past `fold_after` pages, the inserting backend
reads those heap pages, extends the model over them (often by simply carrying
the last segment forward), and moves the frontier. VACUUM does the same.

**Fitting, robust to messy tables.** Each heap page is summarised by its median
key, the longest non-decreasing run of those medians is kept, and segments are
fitted through that run. A batch of rows loaded out of place drops out as a
whole and its rows become exceptions. How tightly the segments follow the data
is chosen from the data.

If the column does not follow physical order at all, every row becomes an
exception and the index is, in effect, a B-tree.

## Files

| file | contents |
|---|---|
| `heapmap.h` | page layouts, the state page, shared declarations |
| `hm_model.c` | evaluating the model, fitting it, deciding which rows it covers |
| `hm_build.c` | `CREATE INDEX` / `REINDEX`, appending segments |
| `hm_insert.c` | inserts, folding the tail into the model, VACUUM |
| `hm_scan.c` | scans (bitmap only, like BRIN) |
| `hm_exc.c` | the exception B+-tree |
| `hm_am.c` | access method handler, options, segment cache, `heapmap_stats()` |
| `test/` | correctness suite |
| `eval/quick_compare.sh` | first comparison with btree and BRIN |

## Running it

```
./cluster.sh init        # private cluster on port 5545, first time only
./cluster.sh install     # build and install the extension
./cluster.sh psql
test/run_tests.sh        # correctness suite, about 2 minutes
eval/quick_compare.sh    # 10M rows against btree and BRIN, about 5 minutes
```

`cluster.sh` uses the system PostgreSQL 18 (`/usr/lib/postgresql/18/bin`). Set
`HM_PG_BIN` to use another build.

Useful while exploring:

```sql
SELECT * FROM heapmap_stats('events_ts');        -- segments, windows, tail, exceptions
SELECT * FROM heapmap_window('events_ts', 123);  -- which heap pages a lookup of key 123 reads
```

Options: `epsilon` (how tightly segments follow the data, 0 = automatic),
`exception_pct` (a page holding less than this percent of a segment's rows is
cut from its window and its rows stored as exceptions, default 1),
`max_window` (rows further than this many pages from the model are always
exceptions, default 64), `fold_after` (default 16) and `keep` (default 4).

Key types: `bigint`, `integer`, `timestamp`, `timestamptz`, `date`.

## Testing

`test/run_tests.sh` compares the exact set of rows the index returns with a
sequential scan, for random equality and range predicates, on fifteen tables:
sorted, jittered, 2% late rows, whole batches out of place, heavy duplicates,
fully random, negative keys, NULLs, empty, one row, one repeated key, and each
key type. The checks are repeated after appends, late inserts, row-at-a-time
inserts, updates that move rows and updates that do not, deletes, VACUUM, reuse
of freed space, and REINDEX. It then runs 16 concurrent clients inserting,
reading and deleting, and finally stops the server with an immediate shutdown
right after a bulk insert and checks again after recovery.

## First results

10M rows, keys increasing with a random gap, `(k bigint, payload int)`, 423 MB
table. PostgreSQL 18.6, i7-13620H, everything cached. Timings are medians of 5
rounds with the indexes taking turns. One run on one machine: treat the timings
as indicative and the sizes and page counts as exact.

| | btree | BRIN | BRIN, 8 pages per range | heapmap |
|---|---|---|---|---|
| Index size | 214 MB | 32 kB | 240 kB | 24 kB |
| Build | 1.83 s | 0.72 s | 0.72 s | 0.81 s |
| Pages read per point lookup | 4 | 131 | 45 | 5 |
| Point lookup, in server | 2.5 µs | 493 µs | 512 µs | 15 µs |
| Range query, about 10,000 rows | 725 µs | 1,222 µs | 1,192 µs | 596 µs |
| Loading 2M more rows | 2.01 s | 0.96 s | 0.87 s | 1.08 s |
| WAL written by that load | 264 MB | 139 MB | 138 MB | 138 MB |

Read plainly: the index is the size of BRIN and about 30 times faster than BRIN
on point lookups. Against the B-tree it is several thousand times smaller, loads
data about twice as fast and is on par for range queries, but a cached point
lookup is about six times slower, because it filters whole heap pages where the
B-tree goes straight to the row.

## Known limits

* **Concurrent writers.** With many sessions appending at once, rows interleave
  across heap pages and a large share end up as exceptions (about 38% in the
  16-client test, which also uses a deliberately small tail). Answers stay
  correct, but the size advantage goes. The planned fix is to keep the newest
  segment open so its window can widen.
* **Point lookups are slower than a B-tree when cached**, as above.
* **Bitmap scans only.** No `ORDER BY` from the index, no unique constraints, no
  index-only scans, one column.
* **Updates.** An update that moves a row to another page costs an exception.
  The index suits append-mostly tables.
* **Build memory.** `CREATE INDEX` holds one 16-byte record per row in memory.
* **The planner** estimates bitmap heap scans as if rows were scattered, so it
  gives up on this index (and on BRIN) for wide ranges sooner than it should.
* Not yet measured: cold-cache lookups, real datasets, tables above 12M rows.
