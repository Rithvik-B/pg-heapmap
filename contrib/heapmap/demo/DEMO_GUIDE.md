# Demo guide: what to run and what to say

Two scripts. `demo/demo.sh` is the main walk-through on a generated event log
(about 3 minutes, 9 steps, Enter between steps). `demo/demo_real.sh` is a
short second act on one day of real GitHub events (about 1 minute). Both
print every SQL statement before running it and measure everything live.

Before the demo, from `contrib/heapmap`:

```
./cluster.sh start                # the private PostgreSQL 18 on port 5545
AUTO=1 demo/demo.sh >/dev/null    # one warm-up run, so caches are warm
eval/gharchive.sh                 # only if the gh_* tables are not there yet
```

Then, for the audience:

```
demo/demo.sh
demo/demo_real.sh
```

## demo/demo.sh, step by step

**Opening line.** "This is an event log: five million rows, timestamps only
move forward. We keep three identical copies of it, each with a different
index on the timestamp: PostgreSQL's B-tree, PostgreSQL's BRIN, and ours."

**1. Load the table.** Nothing to explain. Point out the table size (about
250 MB) so the index sizes in step 2 have a reference.

**2. Build one index on each copy.** The first result. "The B-tree is 107 MB,
because it stores every timestamp with a pointer to its row. BRIN and heapmap
are 24 kB each. BRIN stores a min and max for every 128 pages. heapmap stores
about thirty line segments: for a timestamp, which page is it on, give or take
one page."

**3. Find one event by its timestamp.** Three EXPLAIN plans. Read the
`Buffers` line of each. "B-tree: four pages, three index pages and the row's
page. BRIN: it reads a whole block range, 128 pages, and throws away twenty
thousand rows. heapmap: it reads the three pages its line predicts and
filters a few hundred rows." Then the averaged numbers: pages touched and
microseconds. Be upfront: "The B-tree is still the fastest when everything is
cached, about 7 microseconds against 16. It goes straight to the row, we
filter a page. But we are 30 times faster than BRIN at the same size."

**4. One hour of events.** A range query returning about 2,000 rows. "Here we
are level with the B-tree, slightly ahead, because a range is a run of
consecutive pages and that is exactly what the model gives us."

**5. What is inside the index.** `heapmap_stats` shows the parts: segments,
pages per lookup, tail pages, exception entries. `heapmap_segments` shows the
model itself: "each row is a straight line, timestamp to page number. The
window columns say how far a row may sit from the line. Minus one to plus one
means we read three pages." `heapmap_window` shows what one lookup reads.
This is the slide where the idea becomes concrete; pause here.

**6. Keep loading.** One million more rows into each table, timed, with the
WAL written. "The B-tree writes 140 MB of WAL because every row changes the
index. We write nothing per row; our WAL is the table's own." Index size after:
B-tree grew by 20 MB, heapmap did not grow.

**7. Rows that do not fit the pattern.** A thousand events arrive late with
old timestamps. "Each one is stored exactly, as a small B-tree entry inside
our index. Exception entries went from 117 to 1,117." One of them is looked
up and found. Then an UPDATE moves a row's timestamp forward and it is found
under the new value. "So late rows and updates are correct, they just cost one
small entry each."

**8. Eight sessions inserting at once.** The live table. "Every second, each
column shows how much each index has grown. The B-tree grows by about a
megabyte a second. Ours grows by a few kilobytes." After it ends: "Concurrent
sessions each fill their own page, so neighbouring timestamps are scattered
across several pages. The model's windows widen to cover that scatter, and the
few rows that still do not fit are stored exactly: 0.03 percent of rows."
This is the part that took the most engineering; if asked, the README section
"Concurrent writers" has the three rules.

**9. Is every answer right?** Forty random questions, each answered through
the index and by a full scan of the table. "Forty of forty identical. The test
suite does this on sixteen kinds of tables, after inserts, updates, deletes,
VACUUM, REINDEX, concurrent clients and a crash."

**Summary table.** Leave it on screen. The honest reading: "Same size as
BRIN, 30 times faster than BRIN. Thousands of times smaller than the B-tree,
on par for ranges and loads, about twice as slow for a single cached lookup."

## demo/demo_real.sh

"Generated data is well behaved. Here is a real stream: every public GitHub
event on 1 January 2024, 3.9 million of them, in the order GH Archive
recorded them, indexed on created_at."

The script shows: how out of order the real stream is (0.19 percent of events
are behind an earlier one, by up to an hour), the three index sizes (B-tree
25 MB, heapmap 136 kB), one "events at this second" lookup on each index with
its page count, a five-minute window grouped by event type, and
`heapmap_stats`. The exception count (about 6,400) matches the number of
out-of-order events: "the index stores exactly the rows that really are out
of place, and a model for everything else."

## If something goes wrong

* The cluster is not running: `./cluster.sh start`.
* The extension is missing after a rebuild: `./cluster.sh install`, then
  `DROP EXTENSION heapmap CASCADE; CREATE EXTENSION heapmap;` in psql.
* The demo stops with an error: `demo/sample_output.txt` is a complete run;
  open it and talk through the same numbers.
* The gh_* tables are missing: skip `demo_real.sh` and quote
  `eval/results/gharchive_1day.txt`.
