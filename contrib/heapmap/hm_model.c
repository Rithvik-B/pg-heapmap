// The model: evaluating it, fitting it, and deciding which rows it covers.
//
// Fitting has to survive real tables, where some rows are nowhere near their
// neighbours (late arrivals, backfills, bad timestamps). It works in three steps:
//
//   1. Summarise each heap page by its median key. A few stray rows on a page
//      do not move the median.
//   2. Keep the longest run of pages whose medians never decrease. Pages that
//      are entirely out of place (a batch loaded late) drop out here.
//   3. Fit straight segments through (median key, page number) so that no kept
//      page is more than epsilon pages from its segment.
//
// Then every row is compared with the model. Rows within a segment's window are
// covered; the rest are exceptions.
#include "heapmap.h"

#include <math.h>

#include "access/htup_details.h"
#include "utils/memutils.h"

// Predicted heap page for key. This is the only place the arithmetic lives:
// build, insert and scan must agree to the last bit, or a row could sit just
// outside the window a scan computes for it.
BlockNumber
hm_predict(const HmSegment *seg, int64 key)
{
	double		dx;
	double		p;

	if (key >= seg->start_key)
		dx = (double) ((uint64) key - (uint64) seg->start_key);
	else
		dx = -(double) ((uint64) seg->start_key - (uint64) key);

	p = seg->intercept + seg->slope * dx;
	if (!(p > 0.0))				// also catches NaN
		return 0;
	if (p >= (double) MaxBlockNumber)
		return MaxBlockNumber;
	return (BlockNumber) p;
}

// The segment responsible for key: the last one starting at or below it. Keys
// below every segment use the first.
int
hm_find_segment(const HmSegment *segs, uint32 nsegs, int64 key)
{
	uint32		lo = 0;
	uint32		hi = nsegs;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (segs[mid].start_key <= key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo == 0 ? 0 : (int) (lo - 1);
}

// Pages a covered row with this key can be on. False if there are none.
bool
hm_window(const HmSegment *seg, int64 key, BlockNumber *lo, BlockNumber *hi)
{
	int64		p;
	int64		l;
	int64		h;

	if (seg->min_err > seg->max_err)
		return false;

	p = (int64) hm_predict(seg, key);
	l = p + seg->min_err;
	h = p + seg->max_err;
	if (h < 0)
		return false;
	if (l < 0)
		l = 0;
	if (h > (int64) MaxBlockNumber)
		h = (int64) MaxBlockNumber;
	*lo = (BlockNumber) l;
	*hi = (BlockNumber) h;
	return true;
}

// Pages a covered row with any key in [lo, hi] can be on. Within a segment the
// prediction never decreases, so its two ends bound everything in between.
bool
hm_range_window(const HmSegment *segs, uint32 nsegs, int64 lo, int64 hi,
				BlockNumber *first, BlockNumber *last)
{
	int			a;
	int			b;
	bool		found = false;

	if (nsegs == 0 || lo > hi)
		return false;

	a = hm_find_segment(segs, nsegs, lo);
	b = hm_find_segment(segs, nsegs, hi);

	for (int s = a; s <= b; s++)
	{
		int64		klo = (s == a) ? lo : segs[s].start_key;
		int64		khi = (s == b) ? hi : segs[s + 1].start_key - 1;
		BlockNumber wlo,
					whi,
					unused;

		if (!hm_window(&segs[s], klo, &wlo, &unused))
		{
			// the low end falls off the front; the high end may still be valid
			if (segs[s].min_err > segs[s].max_err)
				continue;
			wlo = 0;
		}
		if (!hm_window(&segs[s], khi, &unused, &whi))
			continue;

		if (!found)
		{
			*first = wlo;
			*last = whi;
			found = true;
		}
		else
		{
			*first = Min(*first, wlo);
			*last = Max(*last, whi);
		}
	}
	return found;
}

int
hm_tuple_cmp_key(const void *a, const void *b)
{
	const HmTuple *x = a;
	const HmTuple *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	if (x->blk != y->blk)
		return x->blk < y->blk ? -1 : 1;
	if (x->off != y->off)
		return x->off < y->off ? -1 : 1;
	return 0;
}

int
hm_tuple_cmp_phys(const void *a, const void *b)
{
	const HmTuple *x = a;
	const HmTuple *y = b;

	if (x->blk != y->blk)
		return x->blk < y->blk ? -1 : 1;
	if (x->off != y->off)
		return x->off < y->off ? -1 : 1;
	return 0;
}

static int
int64_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a;
	int64		y = *(const int64 *) b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

// Lower fence for the tail: a key near the bottom of the keys given, low enough
// that rows arriving slightly late still count as in order, but not dragged down
// by a single stray row. Sorts the array.
int64
hm_choose_fence(int64 *keys, int64 n)
{
	Assert(n > 0);
	qsort(keys, n, sizeof(int64), int64_cmp);
	return keys[n / 20];		// 5th percentile
}

typedef struct Knot
{
	int64		x;				// median key of the page
	double		y;				// the page, measured at its middle
} Knot;

// Steps 1 and 2: one knot per heap page, then the longest non-decreasing run.
// tuples must be in physical order. Returns the number of knots kept.
static int64
make_knots(const HmTuple *tuples, int64 n, Knot **out)
{
	int64		npages = 0;
	int64		cap = 1024;
	int64	   *med = palloc(sizeof(int64) * cap);
	BlockNumber *blk = palloc(sizeof(BlockNumber) * cap);
	int64		buf[MaxHeapTuplesPerPage + 8];
	int64	   *tails;
	int64	   *prev;
	int64		len = 0;
	int64		at;
	Knot	   *knots;
	int64		i = 0;

	while (i < n)
	{
		int64		j = i;
		int			cnt = 0;

		while (j < n && tuples[j].blk == tuples[i].blk)
		{
			if (cnt < (int) lengthof(buf))
				buf[cnt++] = tuples[j].key;
			j++;
		}
		qsort(buf, cnt, sizeof(int64), int64_cmp);

		if (npages == cap)
		{
			cap *= 2;
			med = repalloc_huge(med, sizeof(int64) * cap);
			blk = repalloc_huge(blk, sizeof(BlockNumber) * cap);
		}
		med[npages] = buf[(cnt - 1) / 2];
		blk[npages] = tuples[i].blk;
		npages++;
		i = j;
	}

	if (npages == 0)
	{
		*out = NULL;
		return 0;
	}

	// Longest non-decreasing subsequence, by patience sorting. tails[k] is the
	// page ending the best run of length k+1 found so far.
	tails = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(int64) * npages);
	prev = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(int64) * npages);
	for (int64 p = 0; p < npages; p++)
	{
		int64		lo = 0;
		int64		hi = len;

		while (lo < hi)
		{
			int64		mid = lo + (hi - lo) / 2;

			if (med[tails[mid]] <= med[p])
				lo = mid + 1;
			else
				hi = mid;
		}
		prev[p] = lo > 0 ? tails[lo - 1] : -1;
		tails[lo] = p;
		if (lo == len)
			len++;
	}

	knots = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(Knot) * len);
	at = tails[len - 1];
	for (int64 k = len - 1; k >= 0; k--)
	{
		knots[k].x = med[at];
		knots[k].y = (double) blk[at] + 0.5;
		at = prev[at];
	}

	pfree(med);
	pfree(blk);
	pfree(tails);
	pfree(prev);
	*out = knots;
	return len;
}

static void
emit_segment(HmSegment **segs, int *nsegs, int *cap, int64 start, double slope, double intercept)
{
	HmSegment  *s;

	if (*nsegs == *cap)
	{
		*cap *= 2;
		*segs = repalloc_huge(*segs, sizeof(HmSegment) * *cap);
	}
	s = &(*segs)[(*nsegs)++];
	s->start_key = start;
	s->slope = slope;
	s->intercept = intercept;
	s->min_err = 1;				// covers nothing until hm_classify says otherwise
	s->max_err = 0;
}

// Step 3: segments through the knots, none further than eps pages from a knot.
// Greedy "shrinking cone": keep the range of slopes that still pass within eps
// of every knot since the segment began, and start a new segment when that range
// becomes empty. Within the range, the slope used is the least-squares one.
static int
fit_knots(const Knot *knots, int64 m, double eps, HmSegment **segs_out)
{
	int			cap = 64;
	int			nsegs = 0;
	HmSegment  *segs = palloc(sizeof(HmSegment) * cap);
	bool		open = false;
	int64		x0 = 0;
	double		y0 = 0.0;
	double		slo = 0.0;
	double		shi = 0.0;
	double		sxx = 0.0;
	double		sxy = 0.0;
	int64		j = 0;

#define CLOSE_SEGMENT() \
	emit_segment(&segs, &nsegs, &cap, x0, \
				 sxx > 0.0 ? Min(Max(sxy / sxx, slo), shi) : 0.0, y0)

	while (j < m)
	{
		int64		g = j + 1;
		double		ylo,
					yhi,
					y;
		int64		x = knots[j].x;

		while (g < m && knots[g].x == x)
			g++;
		ylo = knots[j].y;
		yhi = knots[g - 1].y;
		y = (ylo + yhi) / 2.0;

		if (yhi - ylo > 2.0 * eps)
		{
			// One key value fills many pages. No slope can describe that, so it
			// gets a flat segment of its own and the window carries the spread.
			if (open)
			{
				CLOSE_SEGMENT();
				open = false;
			}
			emit_segment(&segs, &nsegs, &cap, x, 0.0, y);
		}
		else if (!open)
		{
			x0 = x;
			y0 = y;
			slo = 0.0;
			shi = INFINITY;
			sxx = sxy = 0.0;
			open = true;
		}
		else
		{
			double		dx = (double) ((uint64) x - (uint64) x0);
			double		nlo = Max(slo, (y - eps - y0) / dx);
			double		nhi = Min(shi, (y + eps - y0) / dx);

			if (nlo > nhi)
			{
				CLOSE_SEGMENT();
				x0 = x;
				y0 = y;
				slo = 0.0;
				shi = INFINITY;
				sxx = sxy = 0.0;
			}
			else
			{
				slo = nlo;
				shi = nhi;
				sxx += dx * dx;
				sxy += dx * (y - y0);
			}
		}
		j = g;
	}
	if (open)
		CLOSE_SEGMENT();
#undef CLOSE_SEGMENT

	*segs_out = segs;
	return nsegs;
}

// What a candidate model costs, measured on a sample of rows sorted by key:
// heap pages read per lookup, plus its size counted as pages. Sets the sample's
// segment windows as a side effect.
static double
model_cost(HmTuple *sample, int64 nsample, HmSegment *segs, int nsegs, const HmOptions *opts)
{
	long double pages = 0.0L;
	int64		exceptions = 0;
	double		bytes_per_row;

	hm_classify(sample, nsample, segs, nsegs, opts, true);
	for (int64 i = 0; i < nsample; i++)
	{
		if (sample[i].exception)
			exceptions++;
		else
		{
			const HmSegment *seg = &segs[hm_find_segment(segs, nsegs, sample[i].key)];

			pages += (long double) (seg->max_err - seg->min_err + 1);
		}
	}
	// 14 bytes per exception, 32 per segment. The exchange rate between size and
	// pages read is the one hm_classify trims windows by: a page is worth reading
	// only if it saves exception_pct percent of the rows from being exceptions.
	bytes_per_row = (14.0 * (double) exceptions + 32.0 * (double) nsegs) / (double) nsample;
	return (double) (pages / (long double) nsample) +
		bytes_per_row / (14.0 * Max(opts->exception_pct, 0.01) / 100.0);
}

// Fit a model to tuples and say which of them it covers.
//
// tuples must arrive in physical order; they are returned sorted by key, with
// exception set on the ones the model does not cover and each segment's window
// set. With has_floor the first segment starts at floor_key + 1, so that it takes
// over exactly the keys above everything the existing model already answers for.
//
// How tightly the segments should follow the page medians depends on how noisy
// the table is: too tight and the fit chases noise, giving many short segments
// with bad slopes. Unless the user fixed epsilon, several values are tried on a
// sample and the cheapest model wins.
int
hm_fit(HmTuple *tuples, int64 n, const HmOptions *opts, bool has_floor,
	   int64 floor_key, HmSegment **segs_out)
{
	static const double candidates[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0};
	Knot	   *knots;
	int64		m = make_knots(tuples, n, &knots);
	HmSegment  *best = NULL;
	int			nbest = 0;

	qsort(tuples, n, sizeof(HmTuple), hm_tuple_cmp_key);

	if (opts->epsilon > 0.0)
		nbest = fit_knots(knots, m, opts->epsilon, &best);
	else
	{
		int64		stride = Max(1, n / 200000);
		int64		nsample = (n + stride - 1) / stride;
		HmTuple    *sample = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(HmTuple) * Max(nsample, 1));
		double		best_cost = 0.0;

		for (int c = 0; c < lengthof(candidates); c++)
		{
			HmSegment  *segs;
			int			nsegs = fit_knots(knots, m, candidates[c], &segs);
			double		cost;

			for (int64 i = 0; i < nsample; i++)
				sample[i] = tuples[i * stride];
			cost = model_cost(sample, nsample, segs, nsegs, opts);

			if (best == NULL || cost < best_cost)
			{
				if (best)
					pfree(best);
				best = segs;
				nbest = nsegs;
				best_cost = cost;
			}
			else
				pfree(segs);
		}
		pfree(sample);
	}

	if (nbest > 0 && has_floor)
	{
		// Slide the first segment's start down to floor_key + 1 along its own line.
		HmSegment  *s = &best[0];
		int64		start = floor_key + 1;

		Assert(start <= s->start_key);
		s->intercept -= s->slope * (double) ((uint64) s->start_key - (uint64) start);
		s->start_key = start;
	}

	hm_classify(tuples, n, best, nbest, opts, true);

	if (knots)
		pfree(knots);
	*segs_out = best;
	return nbest;
}

// Compare tuples (sorted by key) with segs and mark the ones the model does not
// cover. With set_bounds, the segments are new and each one's window is chosen
// here: every page that holds a real share of the segment's rows, but not the
// thinly populated pages at its edges. Without it, the existing windows are used.
void
hm_classify(HmTuple *tuples, int64 n, HmSegment *segs, int nsegs,
			const HmOptions *opts, bool set_bounds)
{
	int			cap = opts->max_window;
	int64	   *hist = set_bounds ? palloc(sizeof(int64) * (2 * cap + 1)) : NULL;
	int64		i = 0;

	if (nsegs == 0)
	{
		for (int64 t = 0; t < n; t++)
			tuples[t].exception = true;
		return;
	}

	for (int s = 0; s < nsegs; s++)
	{
		HmSegment  *seg = &segs[s];
		int64		begin = i;
		int64		end;
		int64		inside = 0;

		// seg owns keys below the next segment's start (the first also owns
		// everything below its own start)
		while (i < n && (s == nsegs - 1 || tuples[i].key < segs[s + 1].start_key))
			i++;
		end = i;

		if (!set_bounds)
		{
			for (int64 t = begin; t < end; t++)
			{
				BlockNumber lo,
							hi;

				tuples[t].exception = !(hm_window(seg, tuples[t].key, &lo, &hi) &&
										tuples[t].blk >= lo && tuples[t].blk <= hi);
			}
			continue;
		}

		memset(hist, 0, sizeof(int64) * (2 * cap + 1));
		for (int64 t = begin; t < end; t++)
		{
			int64		r = (int64) tuples[t].blk - (int64) hm_predict(seg, tuples[t].key);

			if (r >= -cap && r <= cap)
			{
				hist[r + cap]++;
				inside++;
			}
		}

		if (inside == 0)
		{
			seg->min_err = 1;
			seg->max_err = 0;
		}
		else
		{
			int			lo = 0;
			int			hi = 2 * cap;
			double		thin = opts->exception_pct / 100.0 * (double) inside;

			while (hist[lo] == 0)
				lo++;
			while (hist[hi] == 0)
				hi--;

			// Every page in the window is read by every lookup in this segment. A
			// page at the edge holding only a sliver of the rows is not worth
			// that: cut it and store its rows exactly instead.
			while (lo < hi)
			{
				int			drop = hist[lo] <= hist[hi] ? lo : hi;

				if ((double) hist[drop] >= thin)
					break;
				if (drop == lo)
				{
					lo++;
					while (hist[lo] == 0)
						lo++;
				}
				else
				{
					hi--;
					while (hist[hi] == 0)
						hi--;
				}
			}
			seg->min_err = lo - cap;
			seg->max_err = hi - cap;
		}

		for (int64 t = begin; t < end; t++)
		{
			int64		r = (int64) tuples[t].blk - (int64) hm_predict(seg, tuples[t].key);

			tuples[t].exception = (seg->min_err > seg->max_err ||
								   r < seg->min_err || r > seg->max_err);
		}
	}

	if (hist)
		pfree(hist);
}
