\echo Use "CREATE EXTENSION heapmap" to load this file. \quit

CREATE FUNCTION heapmap_handler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE ACCESS METHOD heapmap TYPE INDEX HANDLER heapmap_handler;

-- Integers. The cross-type operators let WHERE k = 5 (an int4 literal) use an
-- index on a bigint column, and the other way round.
CREATE OPERATOR FAMILY heapmap_integer_ops USING heapmap;

CREATE OPERATOR CLASS heapmap_int8_ops
    DEFAULT FOR TYPE int8 USING heapmap FAMILY heapmap_integer_ops AS
    OPERATOR 1 <  (int8, int8),
    OPERATOR 2 <= (int8, int8),
    OPERATOR 3 =  (int8, int8),
    OPERATOR 4 >= (int8, int8),
    OPERATOR 5 >  (int8, int8),
    FUNCTION 1 btint8cmp(int8, int8);

CREATE OPERATOR CLASS heapmap_int4_ops
    DEFAULT FOR TYPE int4 USING heapmap FAMILY heapmap_integer_ops AS
    OPERATOR 1 <  (int4, int4),
    OPERATOR 2 <= (int4, int4),
    OPERATOR 3 =  (int4, int4),
    OPERATOR 4 >= (int4, int4),
    OPERATOR 5 >  (int4, int4),
    FUNCTION 1 btint4cmp(int4, int4);

ALTER OPERATOR FAMILY heapmap_integer_ops USING heapmap ADD
    OPERATOR 1 <  (int8, int4),
    OPERATOR 2 <= (int8, int4),
    OPERATOR 3 =  (int8, int4),
    OPERATOR 4 >= (int8, int4),
    OPERATOR 5 >  (int8, int4),
    FUNCTION 1 (int8, int4) btint84cmp(int8, int4),
    OPERATOR 1 <  (int8, int2),
    OPERATOR 2 <= (int8, int2),
    OPERATOR 3 =  (int8, int2),
    OPERATOR 4 >= (int8, int2),
    OPERATOR 5 >  (int8, int2),
    FUNCTION 1 (int8, int2) btint82cmp(int8, int2),
    OPERATOR 1 <  (int4, int8),
    OPERATOR 2 <= (int4, int8),
    OPERATOR 3 =  (int4, int8),
    OPERATOR 4 >= (int4, int8),
    OPERATOR 5 >  (int4, int8),
    FUNCTION 1 (int4, int8) btint48cmp(int4, int8),
    OPERATOR 1 <  (int4, int2),
    OPERATOR 2 <= (int4, int2),
    OPERATOR 3 =  (int4, int2),
    OPERATOR 4 >= (int4, int2),
    OPERATOR 5 >  (int4, int2),
    FUNCTION 1 (int4, int2) btint42cmp(int4, int2);

-- Times. These are 64-bit (or, for date, 32-bit) integers underneath.
CREATE OPERATOR CLASS heapmap_timestamptz_ops
    DEFAULT FOR TYPE timestamptz USING heapmap AS
    OPERATOR 1 <  (timestamptz, timestamptz),
    OPERATOR 2 <= (timestamptz, timestamptz),
    OPERATOR 3 =  (timestamptz, timestamptz),
    OPERATOR 4 >= (timestamptz, timestamptz),
    OPERATOR 5 >  (timestamptz, timestamptz),
    FUNCTION 1 timestamptz_cmp(timestamptz, timestamptz);

CREATE OPERATOR CLASS heapmap_timestamp_ops
    DEFAULT FOR TYPE timestamp USING heapmap AS
    OPERATOR 1 <  (timestamp, timestamp),
    OPERATOR 2 <= (timestamp, timestamp),
    OPERATOR 3 =  (timestamp, timestamp),
    OPERATOR 4 >= (timestamp, timestamp),
    OPERATOR 5 >  (timestamp, timestamp),
    FUNCTION 1 timestamp_cmp(timestamp, timestamp);

CREATE OPERATOR CLASS heapmap_date_ops
    DEFAULT FOR TYPE date USING heapmap AS
    OPERATOR 1 <  (date, date),
    OPERATOR 2 <= (date, date),
    OPERATOR 3 =  (date, date),
    OPERATOR 4 >= (date, date),
    OPERATOR 5 >  (date, date),
    FUNCTION 1 date_cmp(date, date);

-- What the index looks like right now.
CREATE FUNCTION heapmap_stats(
    index regclass,
    OUT segments bigint,            -- pieces in the model
    OUT avg_window float8,          -- heap pages a lookup reads per segment, on average
    OUT max_window bigint,
    OUT heap_pages bigint,
    OUT frontier_page bigint,       -- heap pages below this are described by the model
    OUT tail_pages bigint,          -- heap pages at or above it, always scanned
    OUT frontier_key bigint,        -- lookups below this key skip the tail
    OUT max_covered_key bigint,
    OUT exception_entries bigint,   -- rows stored exactly
    OUT exception_pages bigint,
    OUT exception_height int,
    OUT model_pages bigint,
    OUT folds bigint,               -- times the tail was added to the model
    OUT index_bytes bigint
) AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Which heap pages a lookup of one key reads (the key as its underlying integer).
CREATE FUNCTION heapmap_window(
    index regclass,
    key bigint,
    OUT first_page bigint,
    OUT last_page bigint,
    OUT scans_tail boolean,
    OUT tail_pages bigint
) AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The model itself, one row per segment. The last one is still open.
CREATE FUNCTION heapmap_segments(
    index regclass,
    OUT segment int,
    OUT start_key bigint,           -- the segment answers for keys from here up to the next start
    OUT pages_per_key float8,       -- slope of its line
    OUT page_at_start float8,       -- heap page its line gives for start_key
    OUT window_from int,            -- a row is at most this far before its predicted page ...
    OUT window_to int,              -- ... and at most this far after
    OUT is_open boolean
) RETURNS SETOF record AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION heapmap_stats(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION heapmap_segments(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION heapmap_window(regclass, bigint) FROM PUBLIC;
