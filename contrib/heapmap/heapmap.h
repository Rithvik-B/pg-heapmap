// heapmap: a learned block-range index for PostgreSQL.
//
// For a column whose values follow the physical order of the table (timestamps
// in a log, serial ids), "key -> heap page" is a smooth increasing function. The
// index stores a piecewise-linear model of that function and nothing per row:
//
//   model      key -> predicted heap page, plus an error window per segment
//   tail       the last few heap pages, still being filled, always scanned
//   exceptions rows that are not where the model says (late arrivals, moved
//              rows), kept exactly as (key, row pointer) in a small B-tree
//
// The rule that makes every answer correct: each indexed row is either
//   (a) in the tail and its key is >= the tail's lower fence, or
//   (b) on a page inside the model's window for its key, or
//   (c) in the exception tree.
// CREATE INDEX establishes this, every insert re-checks it for the new row, and
// "folding" the tail into the model re-checks it for every row it moves.
//
// On disk:
//   block 0    state page (frontier, fences, segment count)
//   block 1    root of the exception tree, never moves
//   block 2..  model pages (chained) and exception tree pages, as allocated
#ifndef HEAPMAP_H
#define HEAPMAP_H

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "utils/rel.h"

#define HM_MAGIC 0x484D4150
#define HM_VERSION 1
#define HM_PAGE_ID 0xFF92

#define HM_STATE_BLKNO 0
#define HM_EXC_ROOT_BLKNO 1
#define HM_FIRST_MODEL_BLKNO 2

// page types
#define HM_F_STATE 0x01
#define HM_F_MODEL 0x02
#define HM_F_EXC_LEAF 0x04
#define HM_F_EXC_INTERNAL 0x08

typedef struct HmPageOpaqueData
{
	BlockNumber next;			// model page: next model page. exception leaf: right sibling
	uint16		nentries;
	uint16		flags;
	uint16		level;			// exception tree: 0 for a leaf
	uint16		page_id;
} HmPageOpaqueData;

typedef HmPageOpaqueData *HmPageOpaque;

#define HmOpaque(page) ((HmPageOpaque) PageGetSpecialPointer(page))
#define HM_CONTENT_SIZE \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(HmPageOpaqueData)))

// One piece of the model. A key k >= start_key (and below the next segment's
// start) is predicted to be on heap page floor(intercept + slope * (k - start_key)).
// Rows the segment covers are within [min_err, max_err] pages of that. A segment
// is never changed once written, so backends can cache it for as long as they like.
typedef struct HmSegment
{
	int64		start_key;
	double		slope;
	double		intercept;
	int32		min_err;
	int32		max_err;		// min_err > max_err means the segment covers nothing
} HmSegment;

#define HM_SEGS_PER_PAGE ((int) (HM_CONTENT_SIZE / sizeof(HmSegment)))

// Exception leaf: two parallel arrays, keys then 6-byte row pointers, sorted by key.
#define HM_EXC_ENTRY_SIZE (sizeof(int64) + sizeof(ItemPointerData))
#define HM_LEAF_CAPACITY ((int) (HM_CONTENT_SIZE / HM_EXC_ENTRY_SIZE))
#define HmLeafKeys(page) ((int64 *) PageGetContents(page))
#define HmLeafTids(page) \
	((ItemPointerData *) (PageGetContents(page) + HM_LEAF_CAPACITY * sizeof(int64)))

// Exception internal page: keys then child block numbers. keys[i] is a lower
// bound for everything under children[i]; keys[0] is not used.
#define HM_INT_ENTRY_SIZE (sizeof(int64) + sizeof(BlockNumber))
#define HM_INT_CAPACITY ((int) (HM_CONTENT_SIZE / HM_INT_ENTRY_SIZE))
#define HmIntKeys(page) ((int64 *) PageGetContents(page))
#define HmIntChildren(page) \
	((BlockNumber *) (PageGetContents(page) + HM_INT_CAPACITY * sizeof(int64)))

// Block 0. Everything a reader needs besides the segments themselves.
typedef struct HmStateData
{
	uint32		magic;
	uint32		version;
	Oid			keytype;			// type of the indexed column
	int16		attnum;				// its attribute number in the table

	// The model.
	uint32		nsegments;
	BlockNumber last_model_blk;		// page holding the newest segment
	bool		has_covered;		// is max_covered_key meaningful
	int64		max_covered_key;	// largest key the model covers; segments added
									// later must start above it

	// The tail: heap pages >= frontier. A row there is covered when its key is
	// >= frontier_key.
	BlockNumber frontier;
	int64		frontier_key;

	// Scans skip the exception tree until it has ever held an entry.
	bool		has_exceptions;

	// Counters, for hm_stats() only.
	uint64		nfolds;
	double		build_tuples;
	double		build_exceptions;
} HmStateData;

#define HmState(page) ((HmStateData *) PageGetContents(page))

// Per-backend copy of the segments, in rd_amcache (one chunk, as relcache
// requires). Segments are only ever appended, so the copy is brought up to date
// by reading the ones it has not seen.
typedef struct HmCache
{
	uint32		nloaded;
	uint32		capacity;
	BlockNumber cur_blk;		// model page holding the last segment loaded
	HmSegment	segs[FLEXIBLE_ARRAY_MEMBER];
} HmCache;

typedef struct HmOptions
{
	int32		vl_len_;
	double		epsilon;		// how closely the model follows the data, in pages; 0 = automatic
	int			max_window;		// rows further than this from the model are exceptions
	double		exception_pct;	// a page holding fewer than this share of a segment's
								// rows is cut from its window; those rows become exceptions
	int			fold_after;		// tail length that triggers a fold
	int			keep;			// pages left in the tail by a fold
} HmOptions;

#define HM_DEFAULT_EPSILON 0.0	// 0: chosen from the data
#define HM_DEFAULT_MAX_WINDOW 64
#define HM_DEFAULT_EXCEPTION_PCT 1.0
#define HM_DEFAULT_FOLD_AFTER 16
#define HM_DEFAULT_KEEP 4
#define HM_MAX_FOLD_BLOCKS 512

// A row as the trainer sees it.
typedef struct HmTuple
{
	int64		key;
	BlockNumber blk;
	OffsetNumber off;
	bool		exception;
} HmTuple;

// hm_am.c
extern void hm_get_options(Relation index, HmOptions *out);
extern bytea *hm_options(Datum reloptions, bool validate);
extern int64 hm_datum_key(Oid type, Datum d);
extern void hm_init_page(Page page, uint16 flags);
extern void hm_read_state(Relation index, HmStateData *state);
extern HmCache *hm_sync_cache(Relation index, const HmStateData *state);

// hm_model.c
extern BlockNumber hm_predict(const HmSegment *seg, int64 key);
extern int	hm_find_segment(const HmSegment *segs, uint32 nsegs, int64 key);
extern bool hm_window(const HmSegment *seg, int64 key, BlockNumber *lo, BlockNumber *hi);
extern bool hm_range_window(const HmSegment *segs, uint32 nsegs, int64 lo, int64 hi,
							BlockNumber *first, BlockNumber *last);
extern int	hm_fit(HmTuple *tuples, int64 n, const HmOptions *opts, bool has_floor,
				   int64 floor_key, HmSegment **segs_out);
extern void hm_classify(HmTuple *tuples, int64 n, HmSegment *segs, int nsegs,
						const HmOptions *opts, bool set_bounds);
extern int	hm_tuple_cmp_key(const void *a, const void *b);
extern int	hm_tuple_cmp_phys(const void *a, const void *b);
extern int64 hm_choose_fence(int64 *keys, int64 n);

// hm_exc.c
extern void hm_exc_init_root(Page page);
extern void hm_exc_insert(Relation index, int64 key, ItemPointer tid);
extern void hm_mark_has_exceptions(Relation index);
extern int64 hm_exc_scan(Relation index, int64 lo, int64 hi, TIDBitmap *tbm);
extern void hm_exc_bulk_load(Relation index, const HmTuple *sorted, int64 n);

// hm_build.c
extern IndexBuildResult *hm_build(Relation heap, Relation index, IndexInfo *indexInfo);
extern void hm_buildempty(Relation index);
extern void hm_append_segments(Relation index, Buffer statebuf, HmStateData *newstate,
							   const HmSegment *segs, int nsegs);

// hm_scan.c
extern IndexScanDesc hm_beginscan(Relation index, int nkeys, int norderbys);
extern void hm_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
					  ScanKey orderbys, int norderbys);
extern int64 hm_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void hm_endscan(IndexScanDesc scan);

// hm_insert.c
extern bool hm_insert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
					  Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged,
					  IndexInfo *indexInfo);
extern bool hm_fold(Relation index, Relation heap, bool wait);
extern IndexBulkDeleteResult *hm_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback, void *cb_state);
extern IndexBulkDeleteResult *hm_vacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);

// First position on a sorted page whose key is >= key (may equal n).
static inline int
hm_lower_bound(const int64 *keys, int n, int64 key)
{
	int			lo = 0;
	int			hi = n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

// First position whose key is > key.
static inline int
hm_upper_bound(const int64 *keys, int n, int64 key)
{
	int			lo = 0;
	int			hi = n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (keys[mid] <= key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

#endif							// HEAPMAP_H
