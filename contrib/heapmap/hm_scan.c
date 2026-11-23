// Scans. Like BRIN, the index answers with a bitmap: whole heap pages for the
// part the model and the tail cover, which the executor then filters row by row,
// and exact row pointers for the exceptions.
#include "heapmap.h"

#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"

typedef struct HmScanOpaqueData
{
	bool		empty;			// the quals cannot match anything
	int64		lo;				// inclusive
	int64		hi;				// inclusive
} HmScanOpaqueData;

typedef HmScanOpaqueData *HmScanOpaque;

IndexScanDesc
hm_beginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);

	scan->opaque = palloc0(sizeof(HmScanOpaqueData));
	return scan;
}

// The value being compared against, as int64. sk_subtype is the type of the
// constant, which differs from the column's for cross-type operators.
static int64
qual_value(ScanKey k, Oid keytype)
{
	Oid			type = OidIsValid(k->sk_subtype) ? k->sk_subtype : keytype;

	return hm_datum_key(type, k->sk_argument);
}

void
hm_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	HmScanOpaque so = (HmScanOpaque) scan->opaque;
	Oid			keytype = TupleDescAttr(RelationGetDescr(scan->indexRelation), 0)->atttypid;

	so->empty = false;
	so->lo = PG_INT64_MIN;
	so->hi = PG_INT64_MAX;

	if (keys && scan->numberOfKeys > 0)
		memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));

	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		k = &scan->keyData[i];
		int64		v;

		if (k->sk_flags & SK_ISNULL)
		{
			so->empty = true;
			continue;
		}
		v = qual_value(k, keytype);

		switch (k->sk_strategy)
		{
			case BTLessStrategyNumber:
				if (v == PG_INT64_MIN)
					so->empty = true;
				else
					so->hi = Min(so->hi, v - 1);
				break;
			case BTLessEqualStrategyNumber:
				so->hi = Min(so->hi, v);
				break;
			case BTEqualStrategyNumber:
				so->lo = Max(so->lo, v);
				so->hi = Min(so->hi, v);
				break;
			case BTGreaterEqualStrategyNumber:
				so->lo = Max(so->lo, v);
				break;
			case BTGreaterStrategyNumber:
				if (v == PG_INT64_MAX)
					so->empty = true;
				else
					so->lo = Max(so->lo, v + 1);
				break;
			default:
				elog(ERROR, "unexpected strategy number %d", k->sk_strategy);
		}
	}
	if (so->lo > so->hi)
		so->empty = true;
}

int64
hm_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	HmScanOpaque so = (HmScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;
	Relation	heap;
	BlockNumber nblocks;
	BlockNumber limit;
	BlockNumber first;
	BlockNumber last;
	HmStateData state;
	HmModel		model;
	int64		pages = 0;
	int64		exact;

	pgstat_count_index_scan(index);
	if (scan->instrument)
		scan->instrument->nsearches++;
	if (so->empty)
		return 0;

	// Open the table before touching the model: taking the lock can process
	// invalidations, which may free the cached segments.
	heap = table_open(IndexGetRelation(RelationGetRelid(index), false), AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(heap);
	table_close(heap, AccessShareLock);

	hm_read_state(index, &state);
	hm_get_model(index, &state, &model);

	// Rows the model covers: only below the frontier.
	limit = Min(state.frontier, nblocks);
	if (limit > 0 &&
		hm_range_window(&model, so->lo, so->hi, &first, &last) &&
		first < limit)
	{
		last = Min(last, limit - 1);
		for (BlockNumber b = first;; b++)
		{
			tbm_add_page(tbm, b);
			pages++;
			if (b == last)
				break;
			CHECK_FOR_INTERRUPTS();
		}
	}

	// Rows in the tail: every tail page, but only for keys at or above its fence.
	if (so->hi >= state.frontier_key)
	{
		for (BlockNumber b = state.frontier; b < nblocks; b++)
		{
			tbm_add_page(tbm, b);
			pages++;
		}
	}

	exact = state.has_exceptions ? hm_exc_scan(index, so->lo, so->hi, tbm) : 0;

	// The return value is only an estimate of the rows found.
	return pages * 10 + exact;
}

void
hm_endscan(IndexScanDesc scan)
{
	pfree(scan->opaque);
}
