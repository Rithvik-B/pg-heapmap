// The exception tree: a small B+-tree of (key, row pointer) for the rows the
// model does not cover.
//
// It is deliberately the textbook structure, since it only ever sees the rare
// rows:
//   * The root stays at block 1 for the life of the index. When it fills, its
//     contents move to a new page and the root becomes that page's parent.
//   * Inserts split full pages on the way down, so a split never has to climb
//     back up, and locks are only ever taken parent before child.
//   * Leaves are chained left to right for range scans.
//   * Pages are never merged or freed. VACUUM empties them in place and REINDEX
//     rebuilds the lot.
// Equal keys may span leaves, so a search starts at the leftmost leaf that can
// hold the key and walks right.
#include "heapmap.h"

#include "miscadmin.h"
#include "nodes/tidbitmap.h"

void
hm_exc_init_root(Page page)
{
	hm_init_page(page, HM_F_EXC_LEAF);
}

static inline bool
page_is_full(Page page)
{
	HmPageOpaque op = HmOpaque(page);

	return op->nentries >= ((op->flags & HM_F_EXC_LEAF) ? HM_LEAF_CAPACITY : HM_INT_CAPACITY);
}

static void
leaf_add(Page page, int64 key, ItemPointer tid)
{
	HmPageOpaque op = HmOpaque(page);
	int64	   *keys = HmLeafKeys(page);
	ItemPointerData *tids = HmLeafTids(page);
	int			n = op->nentries;
	int			at = hm_upper_bound(keys, n, key);

	memmove(keys + at + 1, keys + at, (n - at) * sizeof(int64));
	memmove(tids + at + 1, tids + at, (n - at) * sizeof(ItemPointerData));
	keys[at] = key;
	tids[at] = *tid;
	op->nentries = n + 1;
}

// Child to descend into for an insert of key: the last one whose lower bound is
// <= key.
static inline int
child_for_insert(Page page, int64 key)
{
	return hm_upper_bound(HmIntKeys(page) + 1, HmOpaque(page)->nentries - 1, key);
}

// Child to start a search for key at: the last one whose lower bound is < key,
// so that equal keys left behind by a split are not missed.
static inline int
child_for_search(Page page, int64 key)
{
	return hm_lower_bound(HmIntKeys(page) + 1, HmOpaque(page)->nentries - 1, key);
}

// The root is full. Move everything in it to a new page and make the root an
// internal page with that one child. The caller holds the root exclusively.
static void
raise_root(Relation index, Buffer rootbuf)
{
	Buffer		newbuf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	GenericXLogState *state = GenericXLogStart(index);
	Page		newpage = GenericXLogRegisterBuffer(state, newbuf, GENERIC_XLOG_FULL_IMAGE);
	Page		root = GenericXLogRegisterBuffer(state, rootbuf, 0);
	uint16		level = HmOpaque(root)->level;

	memcpy(newpage, root, BLCKSZ);

	hm_init_page(root, HM_F_EXC_INTERNAL);
	HmOpaque(root)->level = level + 1;
	HmOpaque(root)->nentries = 1;
	HmIntKeys(root)[0] = PG_INT64_MIN;
	HmIntChildren(root)[0] = BufferGetBlockNumber(newbuf);

	GenericXLogFinish(state);
	UnlockReleaseBuffer(newbuf);
}

// Split the full child at slot idx of parent, which has room. Both are held
// exclusively. Returns the new right sibling, locked, and its lowest key.
static Buffer
split_child(Relation index, Buffer parentbuf, int idx, Buffer childbuf, int64 *sep)
{
	Buffer		rightbuf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	GenericXLogState *state = GenericXLogStart(index);
	Page		right = GenericXLogRegisterBuffer(state, rightbuf, GENERIC_XLOG_FULL_IMAGE);
	Page		child = GenericXLogRegisterBuffer(state, childbuf, 0);
	Page		parent = GenericXLogRegisterBuffer(state, parentbuf, 0);
	HmPageOpaque cop = HmOpaque(child);
	HmPageOpaque rop;
	HmPageOpaque pop = HmOpaque(parent);
	int			n = cop->nentries;
	int			keep = n / 2;
	int			moved = n - keep;
	int64	   *pkeys = HmIntKeys(parent);
	BlockNumber *pkids = HmIntChildren(parent);
	int			pn = pop->nentries;

	hm_init_page(right, cop->flags & (HM_F_EXC_LEAF | HM_F_EXC_INTERNAL));
	rop = HmOpaque(right);
	rop->level = cop->level;
	rop->next = cop->next;
	rop->nentries = moved;

	if (cop->flags & HM_F_EXC_LEAF)
	{
		memcpy(HmLeafKeys(right), HmLeafKeys(child) + keep, moved * sizeof(int64));
		memcpy(HmLeafTids(right), HmLeafTids(child) + keep, moved * sizeof(ItemPointerData));
		*sep = HmLeafKeys(right)[0];
	}
	else
	{
		memcpy(HmIntKeys(right), HmIntKeys(child) + keep, moved * sizeof(int64));
		memcpy(HmIntChildren(right), HmIntChildren(child) + keep, moved * sizeof(BlockNumber));
		*sep = HmIntKeys(right)[0];
	}
	cop->nentries = keep;
	cop->next = BufferGetBlockNumber(rightbuf);

	memmove(pkeys + idx + 2, pkeys + idx + 1, (pn - idx - 1) * sizeof(int64));
	memmove(pkids + idx + 2, pkids + idx + 1, (pn - idx - 1) * sizeof(BlockNumber));
	pkeys[idx + 1] = *sep;
	pkids[idx + 1] = BufferGetBlockNumber(rightbuf);
	pop->nentries = pn + 1;

	GenericXLogFinish(state);
	return rightbuf;
}

void
hm_exc_insert(Relation index, int64 key, ItemPointer tid)
{
	Buffer		buf = ReadBuffer(index, HM_EXC_ROOT_BLKNO);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (page_is_full(BufferGetPage(buf)))
		raise_root(index, buf);

	// buf is always held exclusively and has room for one more entry.
	for (;;)
	{
		Page		page = BufferGetPage(buf);
		int			idx;
		Buffer		childbuf;

		if (HmOpaque(page)->flags & HM_F_EXC_LEAF)
		{
			GenericXLogState *state = GenericXLogStart(index);

			leaf_add(GenericXLogRegisterBuffer(state, buf, 0), key, tid);
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
			return;
		}

		idx = child_for_insert(page, key);
		childbuf = ReadBuffer(index, HmIntChildren(page)[idx]);
		LockBuffer(childbuf, BUFFER_LOCK_EXCLUSIVE);

		if (page_is_full(BufferGetPage(childbuf)))
		{
			int64		sep;
			Buffer		rightbuf = split_child(index, buf, idx, childbuf, &sep);

			if (key >= sep)
			{
				UnlockReleaseBuffer(childbuf);
				childbuf = rightbuf;
			}
			else
				UnlockReleaseBuffer(rightbuf);
		}

		UnlockReleaseBuffer(buf);
		buf = childbuf;
	}
}

// Add every exception with a key in [lo, hi] to the bitmap. Returns how many.
int64
hm_exc_scan(Relation index, int64 lo, int64 hi, TIDBitmap *tbm)
{
	Buffer		buf = ReadBuffer(index, HM_EXC_ROOT_BLKNO);
	int64		total = 0;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	while (HmOpaque(BufferGetPage(buf))->flags & HM_F_EXC_INTERNAL)
	{
		Page		page = BufferGetPage(buf);
		Buffer		childbuf = ReadBuffer(index, HmIntChildren(page)[child_for_search(page, lo)]);

		LockBuffer(childbuf, BUFFER_LOCK_SHARE);
		UnlockReleaseBuffer(buf);
		buf = childbuf;
	}

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		HmPageOpaque op = HmOpaque(page);
		int64	   *keys = HmLeafKeys(page);
		ItemPointerData *tids = HmLeafTids(page);
		int			n = op->nentries;
		int			i = hm_lower_bound(keys, n, lo);
		ItemPointerData found[HM_LEAF_CAPACITY];
		int			nfound = 0;
		bool		past_end;
		BlockNumber next = op->next;

		while (i < n && keys[i] <= hi)
			found[nfound++] = tids[i++];
		past_end = (i < n);		// a key above hi: nothing further right can match
		UnlockReleaseBuffer(buf);

		if (nfound > 0)
		{
			tbm_add_tuples(tbm, found, nfound, false);
			total += nfound;
		}
		if (past_end || !BlockNumberIsValid(next))
			break;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBuffer(index, next);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	return total;
}

// One page of a level being bulk loaded.
typedef struct LoadedPage
{
	int64		low;
	BlockNumber blk;
} LoadedPage;

static Buffer
new_page(Relation index, GenericXLogState **state, Page *page, uint16 flags, uint16 level)
{
	Buffer		buf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);

	*state = GenericXLogStart(index);
	*page = GenericXLogRegisterBuffer(*state, buf, GENERIC_XLOG_FULL_IMAGE);
	hm_init_page(*page, flags);
	HmOpaque(*page)->level = level;
	return buf;
}

// Fill an empty tree from exceptions already sorted by key. Used by CREATE
// INDEX, when nothing else can see the index. Pages are left 90% full so the
// first inserts after a build do not split everything at once.
void
hm_exc_bulk_load(Relation index, const HmTuple *sorted, int64 n)
{
	int			per_leaf = HM_LEAF_CAPACITY * 9 / 10;
	int			per_int = HM_INT_CAPACITY * 9 / 10;
	LoadedPage *level;
	int64		count = 0;
	int64		cap;
	uint16		height = 0;
	Buffer		rootbuf;
	GenericXLogState *state;
	Page		page;

	if (n == 0)
		return;

	if (n <= HM_LEAF_CAPACITY)
	{
		rootbuf = ReadBuffer(index, HM_EXC_ROOT_BLKNO);
		LockBuffer(rootbuf, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, rootbuf, GENERIC_XLOG_FULL_IMAGE);
		hm_init_page(page, HM_F_EXC_LEAF);
		for (int64 i = 0; i < n; i++)
		{
			HmLeafKeys(page)[i] = sorted[i].key;
			ItemPointerSet(&HmLeafTids(page)[i], sorted[i].blk, sorted[i].off);
		}
		HmOpaque(page)->nentries = (uint16) n;
		GenericXLogFinish(state);
		UnlockReleaseBuffer(rootbuf);
		return;
	}

	// Leaves. Each is written once the next one exists, so it can link to it.
	cap = n / per_leaf + 2;
	level = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(LoadedPage) * cap);
	{
		Buffer		buf = new_page(index, &state, &page, HM_F_EXC_LEAF, 0);
		int64		i = 0;

		while (i < n)
		{
			int			take = (int) Min((int64) per_leaf, n - i);

			for (int e = 0; e < take; e++)
			{
				HmLeafKeys(page)[e] = sorted[i + e].key;
				ItemPointerSet(&HmLeafTids(page)[e], sorted[i + e].blk, sorted[i + e].off);
			}
			HmOpaque(page)->nentries = (uint16) take;
			level[count].low = sorted[i].key;
			level[count].blk = BufferGetBlockNumber(buf);
			count++;
			i += take;

			if (i < n)
			{
				GenericXLogState *nstate;
				Page		npage;
				Buffer		nbuf = new_page(index, &nstate, &npage, HM_F_EXC_LEAF, 0);

				HmOpaque(page)->next = BufferGetBlockNumber(nbuf);
				GenericXLogFinish(state);
				UnlockReleaseBuffer(buf);
				buf = nbuf;
				state = nstate;
				page = npage;
			}
			else
			{
				GenericXLogFinish(state);
				UnlockReleaseBuffer(buf);
			}
			CHECK_FOR_INTERRUPTS();
		}
	}

	// Internal levels, until what is left fits in the root.
	while (count > HM_INT_CAPACITY)
	{
		int64		out = 0;
		int64		i = 0;

		height++;
		while (i < count)
		{
			int			take = (int) Min((int64) per_int, count - i);
			Buffer		buf = new_page(index, &state, &page, HM_F_EXC_INTERNAL, height);

			for (int e = 0; e < take; e++)
			{
				HmIntKeys(page)[e] = level[i + e].low;
				HmIntChildren(page)[e] = level[i + e].blk;
			}
			HmOpaque(page)->nentries = (uint16) take;
			level[out].low = level[i].low;
			level[out].blk = BufferGetBlockNumber(buf);
			out++;
			i += take;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
		}
		count = out;
	}

	rootbuf = ReadBuffer(index, HM_EXC_ROOT_BLKNO);
	LockBuffer(rootbuf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, rootbuf, GENERIC_XLOG_FULL_IMAGE);
	hm_init_page(page, HM_F_EXC_INTERNAL);
	HmOpaque(page)->level = height + 1;
	for (int64 e = 0; e < count; e++)
	{
		HmIntKeys(page)[e] = level[e].low;
		HmIntChildren(page)[e] = level[e].blk;
	}
	HmOpaque(page)->nentries = (uint16) count;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(rootbuf);

	pfree(level);
}
