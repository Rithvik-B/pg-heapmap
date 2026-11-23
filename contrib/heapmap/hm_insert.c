// Inserts, folding the tail into the model, and VACUUM.
//
// An insert never has to write anything for a row that arrives in order: the row
// lands in the tail with a key at or above the tail's fence, and the tail is
// always scanned. Only a row that is out of place costs an entry in the
// exception tree.
//
// The tail must stay short, because every lookup of a recent key reads all of
// it. So each time the table has grown by a few pages, the inserting backend
// folds the settled part of the tail into the model: it reads those heap pages,
// extends the model over them, and moves the frontier forward. That is the only
// time the index reads the table outside CREATE INDEX.
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

// What a fold needs to know about a tail page besides its rows.
typedef struct PageNote
{
	OffsetNumber maxoff;		// row pointers on the page
	bool		full;			// a row as long as its longest no longer fits
} PageNote;

// Every row on one heap page, visible or not: those with keys at or above the
// fence go to out, the rest to late. Reading more than is live is harmless here:
// the result only decides which pages scans look at and which rows get an
// exception entry.
//
// A row updated in place (a HOT chain) is reported under the row pointer at the
// head of its chain, because that is what index entries must point at.
static void
collect_page(Relation heap, BlockNumber blk, const HmStateData *state,
			 Size save_free, TupleList *out, TupleList *late, PageNote *note)
{
	Buffer		buf = ReadBuffer(heap, blk);
	Page		page;

	note->maxoff = 0;
	note->full = false;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (!PageIsNew(page) && !PageIsEmpty(page))
	{
		OffsetNumber roots[MaxHeapTuplesPerPage];
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		TupleDesc	desc = RelationGetDescr(heap);
		Size		longest = 0;

		heap_get_root_tuples(page, roots);
		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		id = PageGetItemId(page, off);
			HeapTupleData tup;
			Datum		d;
			bool		isnull;
			int64		key;

			if (!ItemIdIsNormal(id))
				continue;
			longest = Max(longest, ItemIdGetLength(id));

			// no root means the row is the dead leftover of an aborted update
			if (!OffsetNumberIsValid(roots[off - 1]))
				continue;

			tup.t_data = (HeapTupleHeader) PageGetItem(page, id);
			tup.t_len = ItemIdGetLength(id);
			tup.t_tableOid = RelationGetRelid(heap);
			ItemPointerSet(&tup.t_self, blk, off);

			d = heap_getattr(&tup, state->attnum, desc, &isnull);
			if (isnull)
				continue;
			key = hm_datum_key(state->keytype, d);
			list_add(key >= state->frontier_key ? out : late, key, blk, roots[off - 1]);
		}

		note->maxoff = maxoff;
		// Whoever fills this page moves on when its next row does not fit. That
		// has very likely happened once the page could not take another row as
		// long as the longest it holds.
		if (longest > 0)
			note->full = PageGetHeapFreeSpace(page) < MAXALIGN(longest) + save_free;
	}
	UnlockReleaseBuffer(buf);
}

// Was this page short of full when last looked at, and has it gained no row
// since? Then nobody is filling it, and the frontier need not wait for it. An
// empty page must also have been left behind by the table's growth; see
// HM_EMPTY_WAIT.
static const HmStall *
find_stall(const HmStateData *state, BlockNumber blk, const PageNote *note)
{
	for (int i = 0; i < state->nstalls; i++)
		if (state->stalls[i].blk == blk)
			return state->stalls[i].maxoff == note->maxoff ? &state->stalls[i] : NULL;
	return NULL;
}

static bool
page_stalled(const HmStateData *state, BlockNumber blk, const PageNote *note, BlockNumber nblocks)
{
	const HmStall *stall = find_stall(state, blk, note);

	if (stall == NULL)
		return false;
	return note->maxoff > 0 || nblocks >= stall->since + HM_EMPTY_WAIT;
}

// Of the rows on pages below `below`, queue the ones the model does not cover
// for the exception tree and note the largest key it does cover.
static double
account(const HmTuple *t, int64 n, BlockNumber below, HmStateData *newstate, TupleList *exc)
{
	double		covered = 0.0;

	for (int64 i = 0; i < n; i++)
	{
		if (t[i].blk >= below)
			continue;
		if (t[i].exception)
		{
			list_add(exc, t[i].key, t[i].blk, t[i].off);
			continue;
		}
		if (!newstate->has_covered || t[i].key > newstate->max_covered_key)
			newstate->max_covered_key = t[i].key;
		newstate->has_covered = true;
		covered += 1.0;
	}
	return covered;
}

// Move the frontier forward over the older part of the tail. Returns false when
// it did not move.
//
// The frontier only passes pages that are settled: full, or left alone since the
// last fold. Under concurrent inserts every session fills a page of its own, so
// the pages at the end of the table fill side by side, and rows with neighbouring
// keys end up many pages apart. Folding a page before it settles would leave the
// model describing rows that are not all there yet.
//
// For the same reason the newest segment stays open. Rows whose keys it already
// answers for keep turning up on later pages, and its window grows to take them
// in, as long as that is cheaper for lookups than storing those rows exactly.
// When new rows stop fitting its line, it is sealed and a new one is fitted.
//
// Why this keeps every answer right while inserts are running: a row's insert
// into the table always finishes before its index insert starts. So any row
// whose index insert has already decided "covered, it is in the tail" is
// physically on its page by the time we read that page here, under the exclusive
// lock on the state page. Each such row is then either still in the tail at or
// above the new fence, inside the model's window, or given an exception entry.
// Windows only ever grow and segments never give up keys, so a row that an
// insert found covered by the model stays covered. Rows whose index insert comes
// later decide against the new state.
bool
hm_fold(Relation index, Relation heap, bool wait)
{
	HmOptions	opts;
	Buffer		statebuf;
	HmStateData state;
	HmStateData newstate;
	BlockNumber nblocks;
	BlockNumber scan_end;
	BlockNumber limit;
	BlockNumber new_frontier;
	uint32		min_step;
	double		seen;
	int			busy = 0;
	bool		partial;
	bool		mature;
	PageNote   *notes;
	TupleList	all;
	TupleList	late;
	TupleList	exc;
	int64		nfold = 0;
	HmTuple    *fit = NULL;
	int64		nfit = 0;
	HmSegment  *fresh = NULL;
	int			nfresh = 0;
	HmSegment  *sealed = NULL;
	int			nsealed = 0;
	double		lambda;

	hm_get_options(index, &opts);
	min_step = (uint32) Max(1, opts.fold_after - opts.keep);
	// what one exception costs, in pages read: the rate windows are trimmed by
	lambda = 100.0 / Max(opts.exception_pct, 0.01);

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

	// Nothing to do while the tail is no longer than it is meant to stay. And an
	// insert does not start a model on a table of a few pages: a fit to so
	// little would be a guess that every later row has to live with.
	nblocks = RelationGetNumberOfBlocks(heap);
	if (nblocks <= state.frontier || nblocks - state.frontier <= (BlockNumber) opts.keep ||
		(!wait && !state.has_open && nblocks < (BlockNumber) opts.fold_after))
	{
		UnlockReleaseBuffer(statebuf);
		return false;
	}

	// Read the tail. Rows below the fence are already exceptions (or their
	// insert is still to come and will make them one), so the model is not
	// shaped for them; they only help to place the new fence.
	// A tail far longer than it should ever get is taken a piece at a time.
	scan_end = nblocks - state.frontier > 2 * (BlockNumber) opts.max_tail
		? state.frontier + 2 * (BlockNumber) opts.max_tail : nblocks;
	partial = scan_end < nblocks;
	notes = palloc(sizeof(PageNote) * (scan_end - state.frontier));
	list_init(&all);
	list_init(&late);
	list_init(&exc);
	for (BlockNumber b = state.frontier; b < scan_end; b++)
		collect_page(heap, b, &state,
					 RelationGetTargetPageFreeSpace(heap, HEAP_DEFAULT_FILLFACTOR),
					 &all, &late, &notes[b - state.frontier]);

	// How far the frontier can go. A page counts as left alone only once enough
	// rows have arrived since it was looked at that any session still filling
	// it would have added one.
	// (Empty pages are not being filled by anyone, so they do not add to the wait.)
	seen = state.rows_folded + (double) (all.n + late.n);
	for (int i = 0; i < state.nstalls; i++)
		if (state.stalls[i].maxoff > 0)
			busy++;
	mature = seen - state.stall_mark >= HM_STALL_ROWS * Min(Max(1, busy), 16);
	if (partial)
		limit = scan_end;
	else
		limit = nblocks - state.frontier > (BlockNumber) opts.keep ? nblocks - opts.keep : state.frontier;
	new_frontier = state.frontier;
	while (new_frontier < limit)
	{
		const PageNote *note = &notes[new_frontier - state.frontier];

		if (!note->full && !(mature && page_stalled(&state, new_frontier, note, nblocks)))
			break;
		new_frontier++;
	}
	// Whatever its pages look like, the tail must not grow without bound: every
	// lookup of a recent key reads all of it.
	if (nblocks - new_frontier > (BlockNumber) opts.max_tail)
		new_frontier = Max(new_frontier, Min(scan_end, nblocks - opts.max_tail));

	if (mature || state.nstalls == 0)
	{
		newstate.nstalls = 0;
		newstate.stall_mark = seen;
		for (BlockNumber b = new_frontier; b < scan_end && newstate.nstalls < HM_MAX_STALLS; b++)
		{
			const PageNote *note = &notes[b - state.frontier];
			const HmStall *before = find_stall(&state, b, note);
			HmStall    *stall;

			if (note->full)
				continue;
			stall = &newstate.stalls[newstate.nstalls++];
			stall->since = before ? before->since : nblocks;
			stall->blk = b;
			stall->maxoff = note->maxoff;
		}
	}
	newstate.frontier = new_frontier;
	// a long tail is read less often, so that folding costs each row about the same
	newstate.fold_step = Max(min_step, (nblocks - new_frontier) / 4);
	if (new_frontier > state.frontier)
		newstate.nfolds = state.nfolds + 1;

	while (nfold < all.n && all.t[nfold].blk < new_frontier)
		nfold++;
	newstate.rows_folded = state.rows_folded + (double) nfold;
	for (int64 i = 0; i < late.n; i++)
		if (late.t[i].blk < new_frontier)
			newstate.rows_folded += 1.0;

	// The new fence. Normally it is the smallest key left in the tail among the
	// rows the old fence let in: all of them stay at or above it, so none needs
	// an exception. But the fence must also stay below where rows are arriving,
	// or every one of them becomes an exception, and a stray row with a key from
	// the future can push it up there. So if the bulk of the tail (its 5th
	// percentile, counting the rows the old fence kept out) lies below the
	// fence, the fence comes down to it, though not below the keys the model
	// has already reached: rows from before that are late, not the bulk.
	// Lowering the fence is always safe: it only widens what the tail answers for.
	if (!partial)
	{
		int64		npool = 0;
		int64	   *pool = palloc(sizeof(int64) * (all.n - nfold + late.n + 1));
		int64		fence = state.frontier_key;
		bool		have = false;

		for (int64 i = nfold; i < all.n; i++)
		{
			fence = have ? Min(fence, all.t[i].key) : all.t[i].key;
			have = true;
			pool[npool++] = all.t[i].key;
		}
		for (int64 i = 0; i < late.n; i++)
			if (late.t[i].blk >= new_frontier)
				pool[npool++] = late.t[i].key;
		if (npool > 0)
		{
			int64		bulk = hm_choose_fence(pool, npool);

			if (state.has_covered)
				bulk = Max(bulk, state.max_covered_key);
			newstate.frontier_key = Min(fence, bulk);
		}
		pfree(pool);
	}

	// From here on the rows are looked at in key order, all of them: the ones
	// being folded and the ones staying in the tail. The tail rows are not
	// covered by the model yet, but they will be folded soon, so the model is
	// shaped for them now. Only rows below the new frontier are accounted for.
	qsort(all.t, all.n, sizeof(HmTuple), hm_tuple_cmp_key);

	if (nfold > 0 && !state.has_open)
	{
		// No model yet: fit one.
		nfit = all.n;
		fit = palloc(sizeof(HmTuple) * nfit);
		memcpy(fit, all.t, sizeof(HmTuple) * nfit);
		qsort(fit, nfit, sizeof(HmTuple), hm_tuple_cmp_phys);
		nfresh = hm_fit(fit, nfit, &opts, false, 0, &fresh);
	}
	else if (nfold > 0)
	{
		HmSegment  *open = &newstate.open;
		int64		floor_key;
		int64		nowned = 0;		// rows a sealed segment answers for
		int64		nlow = 0;
		HmTuple    *lowrows;		// rows the open segment already answers for
		int64		nlowrows;
		HmTuple    *highrows;		// rows with keys above anything covered so far
		int64		nhigh;
		int64		nopen;
		double		had = state.open_rows;
		int32		ext_min = open->min_err;
		int32		ext_max = open->max_err;
		int64		ext_miss;
		bool		seal = false;

		// Existing segments answer for every key up to here, and that can never
		// change, since rows already rely on it.
		floor_key = open->start_key;
		if (state.has_covered && state.max_covered_key > floor_key)
			floor_key = state.max_covered_key;

		if (state.nsegments > 0)
			while (nowned < all.n && all.t[nowned].key < open->start_key)
				nowned++;
		nlow = nowned;
		while (nlow < all.n && all.t[nlow].key <= floor_key)
			nlow++;
		lowrows = all.t + nowned;
		nlowrows = nlow - nowned;
		highrows = all.t + nlow;
		nhigh = all.n - nlow;
		nopen = nlowrows + nhigh;

		if (nowned > 0)
		{
			// sealed windows are final; used at once, before the cache can go away
			HmCache    *cache = hm_sync_cache(index, &state);

			hm_classify(all.t, nowned, cache->segs, (int) state.nsegments, &opts, false);
		}

		// Choice one: the open segment takes all of these rows, its window
		// growing where enough of them need it. The price of a wider window is
		// paid by every row the segment covers, old and new.
		ext_miss = hm_widen(open, lowrows, nopen, &opts, had + (double) nopen, &ext_min, &ext_max);

		if (nhigh > 0)
		{
			int32		tight_min = 1;
			int32		tight_max = 0;
			bool		settled;

			// Is that plainly good enough? It is when nothing had to grow, few
			// rows miss, and these rows alone would not want a much narrower window.
			hm_widen(open, lowrows, nopen, &opts, (double) nopen, &tight_min, &tight_max);
			settled = ext_min == open->min_err && ext_max == open->max_err &&
				(double) ext_miss <= 2.0 * opts.exception_pct / 100.0 * (double) nopen &&
				hm_window_width(ext_min, ext_max) <= hm_window_width(tight_min, tight_max) + 1;

			// A new line needs rows to stand on. A handful above everything
			// covered so far is as likely to be strays as the start of a change.
			if (!settled && nhigh >= 64 && nhigh * 20 >= nopen)
			{
				// Choice two: seal the open segment and fit new ones to the rows
				// above it. Take whichever costs lookups less.
				int32		low_min = open->min_err;
				int32		low_max = open->max_err;
				int64		low_miss;
				double		cost_ext;
				double		cost_seal;
				int			s = 0;

				low_miss = hm_widen(open, lowrows, nlowrows, &opts, had + (double) nlowrows,
									&low_min, &low_max);

				nfit = nhigh;
				fit = palloc(sizeof(HmTuple) * nfit);
				memcpy(fit, highrows, sizeof(HmTuple) * nfit);
				qsort(fit, nfit, sizeof(HmTuple), hm_tuple_cmp_phys);
				nfresh = hm_fit(fit, nfit, &opts, true, floor_key, &fresh);

				cost_ext = (had + (double) nopen) * hm_window_width(ext_min, ext_max) +
					lambda * (double) ext_miss;
				cost_seal = (had + (double) nlowrows) * hm_window_width(low_min, low_max) +
					lambda * (double) low_miss +
					lambda * (double) nfresh * sizeof(HmSegment) / HM_EXC_ENTRY_SIZE;
				for (int64 i = 0; i < nfit; i++)
				{
					while (s + 1 < nfresh && fit[i].key >= fresh[s + 1].start_key)
						s++;
					cost_seal += fit[i].exception ? lambda
						: (double) hm_window_width(fresh[s].min_err, fresh[s].max_err);
				}

				seal = nfresh > 0 && cost_seal < cost_ext;
				if (seal)
				{
					ext_min = low_min;
					ext_max = low_max;
				}
				else
					nfresh = 0;
			}
		}

		open->min_err = ext_min;
		open->max_err = ext_max;
		hm_classify(lowrows, seal ? nlowrows : nopen, open, 1, &opts, false);
		newstate.open_rows = had + account(lowrows, seal ? nlowrows : nopen, new_frontier, &newstate, &exc);
		account(all.t, nowned, new_frontier, &newstate, &exc);
	}

	if (nfresh > 0)
	{
		// The last of the new segments becomes the open one; the one it
		// replaces, and any before it, are sealed.
		sealed = palloc(sizeof(HmSegment) * (nfresh + 1));
		if (state.has_open)
			sealed[nsealed++] = newstate.open;
		for (int i = 0; i < nfresh - 1; i++)
			sealed[nsealed++] = fresh[i];
		newstate.open = fresh[nfresh - 1];
		newstate.has_open = true;
		newstate.open_rows = 0.0;

		account(fit, nfit, new_frontier, &newstate, &exc);
		for (int64 i = nfit - 1; i >= 0; i--)
		{
			if (nfresh > 1 && fit[i].key < newstate.open.start_key)
				break;
			if (fit[i].blk < new_frontier && !fit[i].exception)
				newstate.open_rows += 1.0;
		}
	}

	// Exceptions first, then segments, then the state that makes them count. A
	// crash part way leaves the old state with some harmless extra entries.
	qsort(exc.t, exc.n, sizeof(HmTuple), hm_tuple_cmp_key);
	for (int64 i = 0; i < exc.n; i++)
	{
		ItemPointerData tid;

		// rows of one HOT chain share a key and a head row pointer
		if (i > 0 && hm_tuple_cmp_key(&exc.t[i - 1], &exc.t[i]) == 0)
			continue;
		ItemPointerSet(&tid, exc.t[i].blk, exc.t[i].off);
		hm_exc_insert(index, exc.t[i].key, &tid);
		newstate.has_exceptions = true;
	}
	if (nsealed > 0 || memcmp(&state, &newstate, sizeof(HmStateData)) != 0)
		hm_append_segments(index, statebuf, &newstate, sealed, nsealed);
	UnlockReleaseBuffer(statebuf);

	pfree(all.t);
	pfree(late.t);
	pfree(exc.t);
	pfree(notes);
	if (fit)
		pfree(fit);
	if (fresh)
		pfree(fresh);
	if (sealed)
		pfree(sealed);
	return new_frontier > state.frontier;
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
	int64		key;
	BlockNumber blk;
	bool		covered = false;

	hm_read_state(index, &state);
	blk = ItemPointerGetBlockNumber(ht_ctid);

	// The first row on a page marks one more page's worth of rows arriving
	// somewhere in the table, which makes it a steady clock for folding however
	// the table happens to be growing.
	if (ItemPointerGetOffsetNumber(ht_ctid) == FirstOffsetNumber && blk % state.fold_step == 0)
		hm_fold(index, heap, false);

	if (isnull[0])
		return false;
	key = hm_datum_key(state.keytype, values[0]);

	if (blk >= state.frontier)
		covered = key >= state.frontier_key;
	else if (state.has_open && state.has_covered && key <= state.max_covered_key)
	{
		HmModel		model;
		BlockNumber lo,
					hi;

		hm_get_model(index, &state, &model);
		covered = hm_window(hm_seg(&model, hm_find_segment(&model, key)), key, &lo, &hi) &&
			blk >= lo && blk <= hi;
	}

	if (!covered)
	{
		if (!state.has_exceptions)
			hm_mark_has_exceptions(index);
		hm_exc_insert(index, key, ht_ctid);
	}

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
