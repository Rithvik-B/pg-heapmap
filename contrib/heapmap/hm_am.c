// Access method handler, options, the state page, the segment cache, planner
// costs, and the SQL-callable inspection functions.
#include "heapmap.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(heapmap_handler);
PG_FUNCTION_INFO_V1(heapmap_stats);
PG_FUNCTION_INFO_V1(heapmap_window);

static relopt_kind hm_relopt_kind;

void		_PG_init(void);

void
_PG_init(void)
{
	hm_relopt_kind = add_reloption_kind();
	add_real_reloption(hm_relopt_kind, "epsilon",
					   "How closely the model follows the data, in heap pages (0 picks it from the data)",
					   HM_DEFAULT_EPSILON, 0.0, 1024.0, AccessExclusiveLock);
	add_int_reloption(hm_relopt_kind, "max_window",
					  "Rows further than this many pages from the model are stored as exceptions",
					  HM_DEFAULT_MAX_WINDOW, 1, 65536, AccessExclusiveLock);
	add_real_reloption(hm_relopt_kind, "exception_pct",
					   "A heap page holding less than this percent of a segment's rows is left out of its window, and those rows are stored as exceptions",
					   HM_DEFAULT_EXCEPTION_PCT, 0.0, 100.0, AccessExclusiveLock);
	add_int_reloption(hm_relopt_kind, "fold_after",
					  "Number of new heap pages after which they are added to the model",
					  HM_DEFAULT_FOLD_AFTER, 2, HM_MAX_FOLD_BLOCKS, ShareUpdateExclusiveLock);
	add_int_reloption(hm_relopt_kind, "keep",
					  "Heap pages at the end of the table left out of the model, as they are still filling",
					  HM_DEFAULT_KEEP, 1, 256, ShareUpdateExclusiveLock);
}

bytea *
hm_options(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"epsilon", RELOPT_TYPE_REAL, offsetof(HmOptions, epsilon)},
		{"max_window", RELOPT_TYPE_INT, offsetof(HmOptions, max_window)},
		{"exception_pct", RELOPT_TYPE_REAL, offsetof(HmOptions, exception_pct)},
		{"fold_after", RELOPT_TYPE_INT, offsetof(HmOptions, fold_after)},
		{"keep", RELOPT_TYPE_INT, offsetof(HmOptions, keep)},
	};

	return (bytea *) build_reloptions(reloptions, validate, hm_relopt_kind,
									  sizeof(HmOptions), tab, lengthof(tab));
}

void
hm_get_options(Relation index, HmOptions *out)
{
	HmOptions  *opts = (HmOptions *) index->rd_options;

	if (opts)
		*out = *opts;
	else
	{
		out->epsilon = HM_DEFAULT_EPSILON;
		out->max_window = HM_DEFAULT_MAX_WINDOW;
		out->exception_pct = HM_DEFAULT_EXCEPTION_PCT;
		out->fold_after = HM_DEFAULT_FOLD_AFTER;
		out->keep = HM_DEFAULT_KEEP;
	}
}

// Every supported key type is an integer underneath, and its sort order is the
// integer's, so the whole index works on int64.
int64
hm_datum_key(Oid type, Datum d)
{
	switch (type)
	{
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return DatumGetInt64(d);
		case INT4OID:
		case DATEOID:
			return (int64) DatumGetInt32(d);
		case INT2OID:
			return (int64) DatumGetInt16(d);
		default:
			elog(ERROR, "heapmap does not support type %u", type);
			return 0;
	}
}

// Our pages are plain arrays, so there is no hole between pd_lower and pd_upper
// for WAL to skip.
void
hm_init_page(Page page, uint16 flags)
{
	HmPageOpaque op;

	PageInit(page, BLCKSZ, sizeof(HmPageOpaqueData));
	((PageHeader) page)->pd_lower = ((PageHeader) page)->pd_upper;
	op = HmOpaque(page);
	op->next = InvalidBlockNumber;
	op->nentries = 0;
	op->flags = flags;
	op->level = 0;
	op->page_id = HM_PAGE_ID;
}

void
hm_read_state(Relation index, HmStateData *state)
{
	Buffer		buf = ReadBuffer(index, HM_STATE_BLKNO);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(state, HmState(BufferGetPage(buf)), sizeof(HmStateData));
	UnlockReleaseBuffer(buf);

	if (state->magic != HM_MAGIC || state->version != HM_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid heapmap index",
						RelationGetRelationName(index))));
}

// Returns this backend's copy of the segments, holding at least the
// state->nsegments that state describes. The first state->nsegments segments
// never change once state has been published, so this needs no lock on the
// state page. The pointer is only good until the next relcache invalidation, so
// callers use it straight away and do not keep it across catalog access.
HmCache *
hm_sync_cache(Relation index, const HmStateData *state)
{
	HmCache    *cache = (HmCache *) index->rd_amcache;
	uint32		target = state->nsegments;

	if (cache == NULL || cache->capacity < target)
	{
		uint32		cap = Max(64, target + target / 2);
		HmCache    *grown = MemoryContextAllocHuge(index->rd_indexcxt,
												   offsetof(HmCache, segs) + sizeof(HmSegment) * (Size) cap);

		if (cache)
		{
			memcpy(grown, cache, offsetof(HmCache, segs) + sizeof(HmSegment) * (Size) cache->nloaded);
			pfree(cache);
		}
		else
		{
			grown->nloaded = 0;
			grown->cur_blk = HM_FIRST_MODEL_BLKNO;
		}
		grown->capacity = cap;
		cache = grown;
		index->rd_amcache = cache;
	}

	while (cache->nloaded < target)
	{
		uint32		off = cache->nloaded % HM_SEGS_PER_PAGE;
		uint32		take = Min((uint32) HM_SEGS_PER_PAGE - off, target - cache->nloaded);
		Buffer		buf;

		// cur_blk is the page of the last segment loaded. At a page boundary
		// the next segment is on the page it links to.
		if (off == 0 && cache->nloaded > 0)
		{
			BlockNumber next;

			buf = ReadBuffer(index, cache->cur_blk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			next = HmOpaque(BufferGetPage(buf))->next;
			UnlockReleaseBuffer(buf);
			if (!BlockNumberIsValid(next))
				elog(ERROR, "heapmap index \"%s\": model page chain ends early",
					 RelationGetRelationName(index));
			cache->cur_blk = next;
		}

		buf = ReadBuffer(index, cache->cur_blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(&cache->segs[cache->nloaded],
			   PageGetContents(BufferGetPage(buf)) + (Size) off * sizeof(HmSegment),
			   (Size) take * sizeof(HmSegment));
		UnlockReleaseBuffer(buf);
		cache->nloaded += take;
	}

	return cache;
}

// The index has no entries for covered rows, so a lookup costs almost nothing in
// the index itself. What it costs in the table is the planner's bitmap heap scan
// estimate, driven by the selectivity reported here.
static void
hm_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *startup, Cost *total, Selectivity *sel,
				double *correlation, double *pages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);

	*startup = costs.indexStartupCost;
	*total = costs.indexTotalCost;
	*sel = costs.indexSelectivity;
	*correlation = 1.0;
	*pages = costs.numIndexPages;
}

static bool
hm_validate(Oid opclassoid)
{
	return true;
}

Datum
heapmap_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 5;
	am->amsupport = 1;
	am->amoptsprocnum = 0;
	// The index hands back whole heap pages, so it cannot return rows in order.
	am->amcanorder = false;
	am->amcanorderbyop = false;
	am->amcanhash = false;
	am->amconsistentequality = false;
	am->amconsistentordering = false;
	am->amcanbackward = false;
	am->amcanunique = false;
	am->amcanmulticol = false;
	am->amoptionalkey = false;
	am->amsearcharray = false;
	am->amsearchnulls = false;
	am->amstorage = false;
	am->amclusterable = false;
	am->ampredlocks = false;
	am->amcanparallel = false;
	am->amcanbuildparallel = false;
	am->amcaninclude = false;
	am->amusemaintenanceworkmem = false;
	am->amsummarizing = false;
	am->amparallelvacuumoptions = 0;
	am->amkeytype = InvalidOid;

	am->ambuild = hm_build;
	am->ambuildempty = hm_buildempty;
	am->aminsert = hm_insert;
	am->aminsertcleanup = NULL;
	am->ambulkdelete = hm_bulkdelete;
	am->amvacuumcleanup = hm_vacuumcleanup;
	am->amcanreturn = NULL;
	am->amcostestimate = hm_costestimate;
	am->amgettreeheight = NULL;
	am->amoptions = hm_options;
	am->amproperty = NULL;
	am->ambuildphasename = NULL;
	am->amvalidate = hm_validate;
	am->amadjustmembers = NULL;
	am->ambeginscan = hm_beginscan;
	am->amrescan = hm_rescan;
	am->amgettuple = NULL;		// bitmap scans only, like BRIN
	am->amgetbitmap = hm_getbitmap;
	am->amendscan = hm_endscan;
	am->ammarkpos = NULL;
	am->amrestrpos = NULL;
	am->amestimateparallelscan = NULL;
	am->aminitparallelscan = NULL;
	am->amparallelrescan = NULL;
	am->amtranslatestrategy = NULL;
	am->amtranslatecmptype = NULL;

	PG_RETURN_POINTER(am);
}

static Relation
open_heapmap(Oid relid)
{
	Relation	index = relation_open(relid, AccessShareLock);

	if (index->rd_rel->relkind != RELKIND_INDEX || index->rd_indam->ambuild != hm_build)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a heapmap index", RelationGetRelationName(index))));
	return index;
}

static BlockNumber
heap_blocks_of(Relation index)
{
	Relation	heap = table_open(IndexGetRelation(RelationGetRelid(index), false), AccessShareLock);
	BlockNumber n = RelationGetNumberOfBlocks(heap);

	table_close(heap, AccessShareLock);
	return n;
}

// heapmap_stats(index): what the index currently looks like.
Datum
heapmap_stats(PG_FUNCTION_ARGS)
{
	Relation	index = open_heapmap(PG_GETARG_OID(0));
	BlockNumber heap_pages = heap_blocks_of(index);
	TupleDesc	tupdesc;
	HmStateData state;
	HmCache    *cache;
	BlockNumber nblocks;
	int64		exc_entries = 0,
				exc_pages = 0,
				model_pages = 0;
	int			height = 1;
	double		window_sum = 0.0;
	int64		window_max = 0,
				live_segments = 0;
	Datum		values[14];
	bool		nulls[14] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	hm_read_state(index, &state);
	cache = hm_sync_cache(index, &state);
	for (uint32 s = 0; s < state.nsegments; s++)
	{
		int64		w = (int64) cache->segs[s].max_err - cache->segs[s].min_err + 1;

		if (w <= 0)
			continue;
		live_segments++;
		window_sum += (double) w;
		window_max = Max(window_max, w);
	}

	nblocks = RelationGetNumberOfBlocks(index);
	for (BlockNumber blk = HM_EXC_ROOT_BLKNO; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(index, blk);
		HmPageOpaque op;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		op = HmOpaque(BufferGetPage(buf));
		if (op->flags & HM_F_EXC_LEAF)
		{
			exc_entries += op->nentries;
			exc_pages++;
		}
		else if (op->flags & HM_F_EXC_INTERNAL)
		{
			exc_pages++;
			if (blk == HM_EXC_ROOT_BLKNO)
				height = op->level + 1;
		}
		else if (op->flags & HM_F_MODEL)
			model_pages++;
		UnlockReleaseBuffer(buf);
	}

	values[0] = Int64GetDatum((int64) state.nsegments);
	values[1] = Float8GetDatum(live_segments ? window_sum / (double) live_segments : 0.0);
	values[2] = Int64GetDatum(window_max);
	values[3] = Int64GetDatum((int64) heap_pages);
	values[4] = Int64GetDatum((int64) state.frontier);
	values[5] = Int64GetDatum(heap_pages > state.frontier ? (int64) (heap_pages - state.frontier) : 0);
	values[6] = Int64GetDatum(state.frontier_key);
	if (state.has_covered)
		values[7] = Int64GetDatum(state.max_covered_key);
	else
		nulls[7] = true;
	values[8] = Int64GetDatum(exc_entries);
	values[9] = Int64GetDatum(exc_pages);
	values[10] = Int32GetDatum(height);
	values[11] = Int64GetDatum(model_pages);
	values[12] = Int64GetDatum((int64) state.nfolds);
	values[13] = Int64GetDatum((int64) nblocks * BLCKSZ);

	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

// heapmap_window(index, key): which heap pages a lookup of key reads.
Datum
heapmap_window(PG_FUNCTION_ARGS)
{
	Relation	index = open_heapmap(PG_GETARG_OID(0));
	int64		key = PG_GETARG_INT64(1);
	BlockNumber heap_pages = heap_blocks_of(index);
	TupleDesc	tupdesc;
	HmStateData state;
	HmCache    *cache;
	BlockNumber first,
				last;
	BlockNumber limit;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	hm_read_state(index, &state);
	cache = hm_sync_cache(index, &state);
	limit = Min(state.frontier, heap_pages);

	if (limit > 0 && hm_range_window(cache->segs, state.nsegments, key, key, &first, &last) &&
		first < limit)
	{
		values[0] = Int64GetDatum((int64) first);
		values[1] = Int64GetDatum((int64) Min(last, limit - 1));
	}
	else
		nulls[0] = nulls[1] = true;
	values[2] = BoolGetDatum(key >= state.frontier_key && heap_pages > state.frontier);
	values[3] = Int64GetDatum(key >= state.frontier_key && heap_pages > state.frontier
							  ? (int64) (heap_pages - state.frontier) : 0);

	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
