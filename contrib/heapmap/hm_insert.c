// Inserts, folding the tail into the model, and VACUUM.
//
// An insert never has to write anything for a row that arrives in order: the row
// lands in the tail with a key at or above the tail's fence, and the tail is
// always scanned. Only a row that is out of place costs an entry in the
// exception tree.
//
// The tail must stay short, because every lookup of a recent key reads all of
// it. So when it grows past fold_after pages, the inserting backend folds the
// older part into the model: it reads those heap pages, extends the model over
// them, and moves the frontier forward. That is the only time the index reads
// the table outside CREATE INDEX.
#include "heapmap.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/memutils.h"

typedef struct TupleList
{
	HmTuple    *t;
	int64		n;
	int64		cap;
} TupleList;

static void
list_init(TupleList *l)
{
	l->cap = 4096;
	l->n = 0;
	l->t = palloc(sizeof(HmTuple) * l->cap);
}

static void
list_add(TupleList *l, int64 key, BlockNumber blk, OffsetNumber off)
{
	HmTuple    *t;

	if (l->n == l->cap)
	{
		l->cap *= 2;
		l->t = repalloc_huge(l->t, sizeof(HmTuple) * l->cap);
	}
	t = &l->t[l->n++];
	t->key = key;
	t->blk = blk;
	t->off = off;
	t->exception = false;
}

// Every row on one heap page whose key is >= min_key, visible or not. Reading
// more than is live is harmless here: the result only decides which pages scans
// look at and which rows get an exception entry.
//
// A row updated in place (a HOT chain) is reported under the row pointer at the
// head of its chain, because that is what index entries must point at.
static void
collect_page(Relation heap, BlockNumber blk, const HmStateData *state, int64 min_key,
			 TupleList *out)
{
	Buffer		buf = ReadBuffer(heap, blk);
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (!PageIsNew(page) && !PageIsEmpty(page))
	{
		OffsetNumber roots[MaxHeapTuplesPerPage];
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		TupleDesc	desc = RelationGetDescr(heap);

		heap_get_root_tuples(page, roots);
		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		id = PageGetItemId(page, off);
			HeapTupleData tup;
			Datum		d;
			bool		isnull;
			int64		key;

			// no root means the row is the dead leftover of an aborted update
			if (!ItemIdIsNormal(id) || !OffsetNumberIsValid(roots[off - 1]))
				continue;

			tup.t_data = (HeapTupleHeader) PageGetItem(page, id);
			tup.t_len = ItemIdGetLength(id);
			tup.t_tableOid = RelationGetRelid(heap);
			ItemPointerSet(&tup.t_self, blk, off);

			d = heap_getattr(&tup, state->attnum, desc, &isnull);
			if (isnull)
				continue;
			key = hm_datum_key(state->keytype, d);
			if (key >= min_key)
				list_add(out, key, blk, roots[off - 1]);
		}
	}
	UnlockReleaseBuffer(buf);
}

// Move the frontier forward over the older part of the tail. Returns false when
// nothing was folded.
//
// Why this keeps every answer right, given that inserts are running at the same
// time: a row's insert into the table always finishes before its index insert
// starts. So any row whose index insert has already decided "covered, it is in
// the tail" is physically on its page by the time we read that page here, under
// the exclusive lock on the state page. Each such row is then either still in
// the tail at or above the new fence, inside the model's window, or given an
// exception entry. Rows whose index insert comes later decide against the new
// state.
bool
hm_fold(Relation index, Relation heap, bool wait)
{
	HmOptions	opts;
	Buffer		statebuf;
	HmStateData state;
	HmStateData newstate;
	BlockNumber nblocks;
	BlockNumber new_frontier;
	TupleList	all;
	HmTuple    *fold;
	int64		nfold;
	HmTuple    *rest;
	int64		nrest;
	HmSegment  *oldsegs = NULL;
	uint32		nold;
	HmSegment  *newsegs = NULL;
	int			nnew = 0;
	bool		has_floor = false;
	int64		floor_key = 0;
	int64		nlow = 0;
	int64		nhigh;
	HmTuple    *high;
	ItemPointerData prev_tid;
	int64		prev_key = 0;
	bool		have_prev = false;

	hm_get_options(index, &opts);

	statebuf = ReadBuffer(index, HM_STATE_BLKNO);
	if (wait)
		LockBuffer(statebuf, BUFFER_LOCK_EXCLUSIVE);
	else if (!ConditionalLockBuffer(statebuf))
	{
		// someone else is folding or reading; a later insert will try again
		ReleaseBuffer(statebuf);
		return false;
	}
	memcpy(&state, HmState(BufferGetPage(statebuf)), sizeof(HmStateData));
	newstate = state;

	nblocks = RelationGetNumberOfBlocks(heap);
	if (nblocks <= state.frontier || nblocks - state.frontier <= (BlockNumber) opts.keep)
	{
		UnlockReleaseBuffer(statebuf);
		return false;
	}

	// Rows below the fence are already exceptions (or their insert is still to
	// come and will make them one), so they are not read.
	list_init(&all);
	if (nblocks - state.frontier > HM_MAX_FOLD_BLOCKS)
	{
		// Too long a tail to weigh up in one go: take a fixed bite and leave the
		// fence where it is.
		new_frontier = state.frontier + HM_MAX_FOLD_BLOCKS;
		for (BlockNumber b = state.frontier; b < new_frontier; b++)
			collect_page(heap, b, &state, state.frontier_key, &all);
		nfold = all.n;
	}
	else
	{
		for (BlockNumber b = state.frontier; b < nblocks; b++)
			collect_page(heap, b, &state, state.frontier_key, &all);

		new_frontier = nblocks - opts.keep;
		nfold = 0;
		while (nfold < all.n && all.t[nfold].blk < new_frontier)
			nfold++;

		// The new fence is the smallest key left in the tail. Every tail row
		// stays at or above it, so none of them needs an exception.
		if (nfold < all.n)
		{
			int64		fence = all.t[nfold].key;

			for (int64 i = nfold + 1; i < all.n; i++)
				fence = Min(fence, all.t[i].key);
			newstate.frontier_key = fence;
		}
	}
	fold = all.t;
	rest = all.t + nfold;
	nrest = all.n - nfold;
	(void) rest;
	(void) nrest;

	// Work on a private copy of the existing segments: the cache can be freed
	// under us by anything that processes invalidations.
	nold = state.nsegments;
	if (nold > 0)
	{
		HmCache    *cache = hm_sync_cache(index, &state);

		oldsegs = palloc(sizeof(HmSegment) * nold);
		memcpy(oldsegs, cache->segs, sizeof(HmSegment) * nold);

		// Existing segments answer for every key up to here, and that can never
		// change, since rows already rely on it.
		has_floor = true;
		floor_key = oldsegs[nold - 1].start_key;
		if (state.has_covered && state.max_covered_key > floor_key)
			floor_key = state.max_covered_key;
	}

	qsort(fold, nfold, sizeof(HmTuple), hm_tuple_cmp_key);
	if (has_floor)
	{
		while (nlow < nfold && fold[nlow].key <= floor_key)
			nlow++;
		hm_classify(fold, nlow, oldsegs, nold, &opts, false);
	}
	high = fold + nlow;
	nhigh = nfold - nlow;

	if (nhigh > 0)
	{
		bool		extend = false;

		if (has_floor)
		{
			// Does the newest segment, carried forward, already describe these
			// rows? If so the model does not need to grow at all.
			int64		misses = 0;

			hm_classify(high, nhigh, &oldsegs[nold - 1], 1, &opts, false);
			for (int64 i = 0; i < nhigh; i++)
				if (high[i].exception)
					misses++;
			extend = (double) misses <= 2.0 * opts.exception_pct / 100.0 * (double) nhigh;
		}

		if (!extend)
		{
			qsort(high, nhigh, sizeof(HmTuple), hm_tuple_cmp_phys);
			nnew = hm_fit(high, nhigh, &opts, has_floor, floor_key, &newsegs);
		}
	}

	newstate.frontier = new_frontier;
	newstate.nfolds = state.nfolds + 1;
	for (int64 i = 0; i < nfold; i++)
	{
		if (fold[i].exception)
		{
			newstate.has_exceptions = true;
			continue;
		}
		if (!newstate.has_covered || fold[i].key > newstate.max_covered_key)
			newstate.max_covered_key = fold[i].key;
		newstate.has_covered = true;
	}

	// Exceptions first, then segments, then the state that makes them count. A
	// crash part way leaves the old state with some harmless extra entries.
	for (int64 i = 0; i < nfold; i++)
	{
		ItemPointerData tid;

		if (!fold[i].exception)
			continue;
		ItemPointerSet(&tid, fold[i].blk, fold[i].off);
		// rows of one HOT chain share a key and a head row pointer
		if (have_prev && prev_key == fold[i].key && ItemPointerEquals(&prev_tid, &tid))
			continue;
		hm_exc_insert(index, fold[i].key, &tid);
		prev_tid = tid;
		prev_key = fold[i].key;
		have_prev = true;
	}
	hm_append_segments(index, statebuf, &newstate, newsegs, nnew);
	UnlockReleaseBuffer(statebuf);

	pfree(all.t);
	if (oldsegs)
		pfree(oldsegs);
	if (newsegs)
		pfree(newsegs);
	return true;
}

// The first exception an index ever gets: tell scans the tree is worth reading.
// A scan that read the state before this cannot need the entry that follows,
// because the row it belongs to is not committed yet.
void
hm_mark_has_exceptions(Relation index)
{
	Buffer		statebuf = ReadBuffer(index, HM_STATE_BLKNO);

	LockBuffer(statebuf, BUFFER_LOCK_EXCLUSIVE);
	if (!HmState(BufferGetPage(statebuf))->has_exceptions)
	{
		GenericXLogState *xs = GenericXLogStart(index);
		Page		page = GenericXLogRegisterBuffer(xs, statebuf, 0);

		HmState(page)->has_exceptions = true;
		GenericXLogFinish(xs);
	}
	UnlockReleaseBuffer(statebuf);
}

bool
hm_insert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
		  Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	HmStateData state;
	HmOptions	opts;
	int64		key;
	BlockNumber blk;
	bool		covered = false;

	if (isnull[0])
		return false;

	hm_read_state(index, &state);
	key = hm_datum_key(state.keytype, values[0]);
	blk = ItemPointerGetBlockNumber(ht_ctid);

	if (blk >= state.frontier)
		covered = key >= state.frontier_key;
	else if (state.nsegments > 0 && state.has_covered && key <= state.max_covered_key)
	{
		HmCache    *cache = hm_sync_cache(index, &state);
		const HmSegment *seg = &cache->segs[hm_find_segment(cache->segs, state.nsegments, key)];
		BlockNumber lo,
					hi;

		covered = hm_window(seg, key, &lo, &hi) && blk >= lo && blk <= hi;
	}

	if (!covered)
	{
		if (!state.has_exceptions)
			hm_mark_has_exceptions(index);
		hm_exc_insert(index, key, ht_ctid);
	}

	hm_get_options(index, &opts);
	if (blk >= state.frontier &&
		blk - state.frontier >= (BlockNumber) Max(opts.fold_after, opts.keep + 1))
		hm_fold(index, heap, false);

	return false;
}

// Remove exception entries for dead rows. Covered rows have no entries, so
// there is nothing else to clean.
IndexBulkDeleteResult *
hm_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *cb_state)
{
	Relation	index = info->index;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));

	for (BlockNumber blk = HM_EXC_ROOT_BLKNO; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		HmPageOpaque op;
		int			n;
		int			dead = 0;
		bool		is_dead[HM_LEAF_CAPACITY];

		vacuum_delay_point(false);

		buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL, info->strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		op = HmOpaque(page);

		if (PageIsNew(page) || !(op->flags & HM_F_EXC_LEAF))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		n = op->nentries;
		for (int i = 0; i < n; i++)
		{
			is_dead[i] = callback(&HmLeafTids(page)[i], cb_state);
			if (is_dead[i])
				dead++;
		}

		if (dead > 0)
		{
			GenericXLogState *state = GenericXLogStart(index);
			Page		wpage = GenericXLogRegisterBuffer(state, buf, 0);
			int64	   *keys = HmLeafKeys(wpage);
			ItemPointerData *tids = HmLeafTids(wpage);
			int			kept = 0;

			for (int i = 0; i < n; i++)
			{
				if (is_dead[i])
					continue;
				keys[kept] = keys[i];
				tids[kept] = tids[i];
				kept++;
			}
			HmOpaque(wpage)->nentries = kept;
			GenericXLogFinish(state);
		}

		stats->tuples_removed += dead;
		UnlockReleaseBuffer(buf);
	}

	stats->num_pages = nblocks;
	return stats;
}

IndexBulkDeleteResult *
hm_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only)
		return stats;

	// A good moment to bring the model up to date with the table.
	while (hm_fold(info->index, info->heaprel, true))
		vacuum_delay_point(false);

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	stats->num_index_tuples = info->num_heap_tuples;
	stats->estimated_count = true;
	return stats;
}
