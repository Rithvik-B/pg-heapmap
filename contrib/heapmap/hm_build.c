// CREATE INDEX / REINDEX: read the table once, fit the model, write the pages.
#include "heapmap.h"

#include "access/tableam.h"
#include "catalog/pg_am.h"
#include "miscadmin.h"
#include "storage/bulk_write.h"
#include "utils/memutils.h"

typedef struct BuildState
{
	HmTuple    *tuples;
	int64		n;
	int64		cap;
	Oid			keytype;
} BuildState;

static void
build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
			   bool tupleIsAlive, void *state)
{
	BuildState *bs = (BuildState *) state;
	HmTuple    *t;

	if (isnull[0])
		return;
	if (bs->n == bs->cap)
	{
		bs->cap *= 2;
		bs->tuples = repalloc_huge(bs->tuples, sizeof(HmTuple) * bs->cap);
	}
	t = &bs->tuples[bs->n++];
	t->key = hm_datum_key(bs->keytype, values[0]);
	t->blk = ItemPointerGetBlockNumber(tid);
	t->off = ItemPointerGetOffsetNumber(tid);
	t->exception = false;
}

static void
init_state(Relation index, const HmOptions *opts, HmStateData *state)
{
	memset(state, 0, sizeof(HmStateData));
	state->magic = HM_MAGIC;
	state->version = HM_VERSION;
	state->keytype = TupleDescAttr(RelationGetDescr(index), 0)->atttypid;
	state->attnum = index->rd_index->indkey.values[0];
	state->nsegments = 0;
	state->last_model_blk = HM_FIRST_MODEL_BLKNO;
	state->has_open = false;
	state->has_covered = false;
	state->max_covered_key = PG_INT64_MIN;
	state->frontier = 0;
	state->frontier_key = PG_INT64_MIN;	// an empty index: every row is in the tail
	state->fold_step = (uint32) Max(1, opts->fold_after - opts->keep);
	state->rows_folded = 0.0;
	state->stall_mark = 0.0;
	state->nstalls = 0;
}

// Append sealed segments to the model and publish newstate. The caller holds the
// state page exclusively and has filled in every field of newstate except the
// sealed segment count and last model page, which are advanced here.
//
// Segments go in before the state that counts them. After a crash in between,
// the state still has the old count, readers ignore what lies beyond it, and the
// next append overwrites it.
void
hm_append_segments(Relation index, Buffer statebuf, HmStateData *newstate,
				   const HmSegment *segs, int nsegs)
{
	uint32		have = newstate->nsegments;
	BlockNumber blk = newstate->last_model_blk;
	int			done = 0;
	GenericXLogState *state;
	Page		page;

	while (done < nsegs)
	{
		uint32		off = have % HM_SEGS_PER_PAGE;
		int			take = Min(HM_SEGS_PER_PAGE - (int) off, nsegs - done);
		Buffer		buf;

		if (off == 0 && have > 0)
		{
			// the last page is full: move to the page after it, making one if needed
			Buffer		prev = ReadBuffer(index, blk);
			BlockNumber next;

			LockBuffer(prev, BUFFER_LOCK_EXCLUSIVE);
			next = HmOpaque(BufferGetPage(prev))->next;
			if (!BlockNumberIsValid(next))
			{
				Page		ppage;

				buf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
				state = GenericXLogStart(index);
				page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
				hm_init_page(page, HM_F_MODEL);
				ppage = GenericXLogRegisterBuffer(state, prev, 0);
				next = BufferGetBlockNumber(buf);
				HmOpaque(ppage)->next = next;
				GenericXLogFinish(state);
				UnlockReleaseBuffer(buf);
			}
			UnlockReleaseBuffer(prev);
			blk = next;
		}

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		memcpy(PageGetContents(page) + (Size) off * sizeof(HmSegment),
			   segs + done, (Size) take * sizeof(HmSegment));
		HmOpaque(page)->nentries = (uint16) (off + take);
		GenericXLogFinish(state);
		UnlockReleaseBuffer(buf);

		done += take;
		have += take;
	}

	newstate->nsegments = have;
	newstate->last_model_blk = blk;

	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, statebuf, 0);
	memcpy(HmState(page), newstate, sizeof(HmStateData));
	GenericXLogFinish(state);
}

// The three pages every index has: state, exception root, first model page.
// Returns the state page, locked exclusively.
static Buffer
write_skeleton(Relation index, const HmStateData *state)
{
	Buffer		statebuf;
	static const uint16 flags[] = {HM_F_STATE, HM_F_EXC_LEAF, HM_F_MODEL};

	statebuf = InvalidBuffer;
	for (int i = 0; i < 3; i++)
	{
		Buffer		buf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
		GenericXLogState *xs = GenericXLogStart(index);
		Page		page = GenericXLogRegisterBuffer(xs, buf, GENERIC_XLOG_FULL_IMAGE);

		if (BufferGetBlockNumber(buf) != (BlockNumber) i)
			elog(ERROR, "unexpected block %u while creating heapmap index", BufferGetBlockNumber(buf));

		hm_init_page(page, flags[i]);
		if (i == HM_STATE_BLKNO)
			memcpy(HmState(page), state, sizeof(HmStateData));
		GenericXLogFinish(xs);

		if (i == HM_STATE_BLKNO)
			statebuf = buf;
		else
			UnlockReleaseBuffer(buf);
	}
	return statebuf;
}

// For unlogged tables: the same skeleton in the init fork.
void
hm_buildempty(Relation index)
{
	BulkWriteState *bulk = smgr_bulk_start_rel(index, INIT_FORKNUM);
	HmStateData state;
	HmOptions	opts;
	static const uint16 flags[] = {HM_F_STATE, HM_F_EXC_LEAF, HM_F_MODEL};

	hm_get_options(index, &opts);
	init_state(index, &opts, &state);
	for (int i = 0; i < 3; i++)
	{
		BulkWriteBuffer buf = smgr_bulk_get_buf(bulk);
		Page		page = (Page) buf;

		hm_init_page(page, flags[i]);
		if (i == HM_STATE_BLKNO)
			memcpy(HmState(page), &state, sizeof(HmStateData));
		smgr_bulk_write(bulk, i, buf, true);
	}
	smgr_bulk_finish(bulk);
}

IndexBuildResult *
hm_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result = palloc(sizeof(IndexBuildResult));
	HmOptions	opts;
	HmStateData state;
	BuildState	bs;
	double		heap_tuples;
	BlockNumber nblocks;
	BlockNumber frontier;
	int64		nmodel;
	int64		ntail;
	HmSegment  *segs = NULL;
	int			nsegs = 0;
	HmTuple    *exc;
	int64		nexc = 0;
	Buffer		statebuf;
	bool		sorted = true;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data", RelationGetRelationName(index));
	if (heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("heapmap indexes need a table that uses the heap access method")));
	if (indexInfo->ii_IndexAttrNumbers[0] == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("heapmap indexes cannot be built on an expression")));

	hm_get_options(index, &opts);
	init_state(index, &opts, &state);

	bs.keytype = state.keytype;
	bs.cap = Max(1024, (int64) heap->rd_rel->reltuples + 1);
	bs.n = 0;
	bs.tuples = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(HmTuple) * bs.cap);

	// allow_sync = false: start at block 0, so rows arrive in physical order
	heap_tuples = table_index_build_scan(heap, index, indexInfo, false, true,
										 build_callback, &bs, NULL);
	nblocks = RelationGetNumberOfBlocks(heap);

	for (int64 i = 1; i < bs.n && sorted; i++)
		sorted = hm_tuple_cmp_phys(&bs.tuples[i - 1], &bs.tuples[i]) <= 0;
	if (!sorted)
		qsort(bs.tuples, bs.n, sizeof(HmTuple), hm_tuple_cmp_phys);

	// The last few pages with rows are still being filled, so they start out as
	// the tail. So do the empty pages a bulk load leaves after them: PostgreSQL
	// fills those later, with whatever keys are arriving then.
	if (bs.n > 0)
		nblocks = bs.tuples[bs.n - 1].blk + 1;
	frontier = nblocks > (BlockNumber) opts.keep ? nblocks - opts.keep : 0;
	nmodel = 0;
	while (nmodel < bs.n && bs.tuples[nmodel].blk < frontier)
		nmodel++;
	ntail = bs.n - nmodel;

	if (nmodel > 0)
	{
		// fits the model, sorts these rows by key and marks the ones it does not cover
		nsegs = hm_fit(bs.tuples, nmodel, &opts, false, 0, &segs);

		for (int64 i = 0; i < nmodel; i++)
		{
			if (bs.tuples[i].exception)
				continue;
			if (!state.has_covered || bs.tuples[i].key > state.max_covered_key)
				state.max_covered_key = bs.tuples[i].key;
			state.has_covered = true;
		}

		// The newest segment stays open, in the state page.
		if (nsegs > 0)
		{
			state.has_open = true;
			state.open = segs[--nsegs];
			state.open_rows = 0.0;
			for (int64 i = nmodel - 1; i >= 0; i--)
			{
				// an only segment also owns the keys below its start
				if (nsegs > 0 && bs.tuples[i].key < state.open.start_key)
					break;
				if (!bs.tuples[i].exception)
					state.open_rows += 1.0;
			}
		}
	}

	// The tail's lower fence: its smallest key, unless that key is a stray far
	// below the rest, in which case the fence goes at the 5th percentile and the
	// tail rows below it become exceptions.
	if (ntail > 0)
	{
		int64	   *keys = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(int64) * ntail);
		int64		low;
		int64		high;

		for (int64 i = 0; i < ntail; i++)
			keys[i] = bs.tuples[nmodel + i].key;
		low = hm_choose_fence(keys, ntail);	// sorts keys
		high = keys[ntail - 1 - ntail / 20];
		if ((uint64) low - (uint64) keys[0] <= (uint64) high - (uint64) low)
			low = keys[0];
		state.frontier_key = low;
		pfree(keys);
		for (int64 i = nmodel; i < bs.n; i++)
			bs.tuples[i].exception = bs.tuples[i].key < state.frontier_key;
	}
	else if (state.has_covered)
	{
		// No rows in the tail yet. Start the fence a little below the newest
		// covered key, so rows arriving slightly out of order still qualify.
		state.frontier_key = state.max_covered_key;
		for (int64 i = 0; i < nmodel; i++)
		{
			if (!bs.tuples[i].exception && bs.tuples[i].blk + opts.keep >= frontier &&
				bs.tuples[i].key < state.frontier_key)
				state.frontier_key = bs.tuples[i].key;
		}
	}
	state.frontier = frontier;

	for (int64 i = 0; i < bs.n; i++)
		if (bs.tuples[i].exception)
			nexc++;
	exc = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(HmTuple) * Max(nexc, 1));
	nexc = 0;
	for (int64 i = 0; i < bs.n; i++)
		if (bs.tuples[i].exception)
			exc[nexc++] = bs.tuples[i];
	qsort(exc, nexc, sizeof(HmTuple), hm_tuple_cmp_key);

	state.build_tuples = (double) bs.n;
	state.build_exceptions = (double) nexc;
	state.has_exceptions = nexc > 0;

	statebuf = write_skeleton(index, &state);
	hm_append_segments(index, statebuf, &state, segs, nsegs);
	UnlockReleaseBuffer(statebuf);
	hm_exc_bulk_load(index, exc, nexc);

	result->heap_tuples = heap_tuples;
	result->index_tuples = (double) bs.n;

	pfree(bs.tuples);
	pfree(exc);
	if (segs)
		pfree(segs);
	return result;
}
