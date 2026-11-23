-- hm_check(table, n): run n random predicates on column k through the heapmap
-- index and compare the exact set of rows with a sequential scan. Raises an
-- exception on the first difference. Works for integer, date and timestamp keys.
CREATE OR REPLACE FUNCTION hm_row_set(tbl regclass, pred text) RETURNS text AS $$
DECLARE r text;
BEGIN
    EXECUTE format('SELECT count(*) || '':'' || coalesce(md5(string_agg(ctid::text, '','' ORDER BY ctid)), '''')
                    FROM %s WHERE %s', tbl, pred) INTO r;
    RETURN r;
END $$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION hm_plan_uses_index(tbl regclass, pred text) RETURNS boolean AS $$
DECLARE line text; found boolean := false;
BEGIN
    FOR line IN EXECUTE format('EXPLAIN SELECT ctid FROM %s WHERE %s', tbl, pred) LOOP
        IF line LIKE '%Bitmap Index Scan on%' THEN
            found := true;
        END IF;
    END LOOP;
    RETURN found;
END $$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION hm_check_pred(tbl regclass, pred text) RETURNS void AS $$
DECLARE expected text; got text;
BEGIN
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    expected := hm_row_set(tbl, pred);

    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    IF NOT hm_plan_uses_index(tbl, pred) THEN
        RAISE EXCEPTION 'index not used: % on %', pred, tbl;
    END IF;
    got := hm_row_set(tbl, pred);
    IF got IS DISTINCT FROM expected THEN
        RAISE EXCEPTION 'index scan differs on % WHERE %: expected %, got %', tbl, pred, expected, got;
    END IF;
    PERFORM set_config('enable_seqscan', 'on', true);
END $$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION hm_check(tbl regclass, n int) RETURNS text AS $$
DECLARE
    typ text;
    step text;
    keys text[];
    a text; b text; tmp text;
    cnt int := 0;
    nkeys int;
    swap boolean;
BEGIN
    SELECT format_type(atttypid, NULL) INTO typ FROM pg_attribute WHERE attrelid = tbl AND attname = 'k';
    step := CASE WHEN typ LIKE 'timestamp%' THEN 'interval ''1 microsecond''' ELSE '1' END;

    IF typ = 'bigint' THEN
        PERFORM hm_check_pred(tbl, 'k = 9223372036854775807');
        PERFORM hm_check_pred(tbl, 'k = -9223372036854775808');
        PERFORM hm_check_pred(tbl, 'k > 9223372036854775807');
        PERFORM hm_check_pred(tbl, 'k < -9223372036854775808');
        PERFORM hm_check_pred(tbl, 'k = 5');
        PERFORM hm_check_pred(tbl, 'k = 5::int2');
        PERFORM hm_check_pred(tbl, 'k >= 5 AND k < 500::int4');
    END IF;

    EXECUTE format('SELECT array_agg(k::text) FROM (SELECT k FROM %s WHERE k IS NOT NULL ORDER BY random() LIMIT 2000) s', tbl)
        INTO keys;
    IF keys IS NULL THEN
        RETURN format('%s: ok (no keys)', tbl);
    END IF;
    nkeys := array_length(keys, 1);

    -- the oldest and newest keys, where the model's edges and the tail are
    EXECUTE format('SELECT min(k)::text, max(k)::text FROM %s', tbl) INTO a, b;
    PERFORM hm_check_pred(tbl, format('k = %L::%s', a, typ));
    PERFORM hm_check_pred(tbl, format('k = %L::%s', b, typ));
    PERFORM hm_check_pred(tbl, format('k <= %L::%s', a, typ));
    PERFORM hm_check_pred(tbl, format('k >= %L::%s', b, typ));

    FOR i IN 1..n LOOP
        a := keys[1 + floor(random() * nkeys)::int];
        b := keys[1 + floor(random() * nkeys)::int];
        EXECUTE format('SELECT %L::%s > %L::%s', a, typ, b, typ) INTO swap;
        IF swap THEN tmp := a; a := b; b := tmp; END IF;

        -- present keys, and neighbours that are usually absent
        PERFORM hm_check_pred(tbl, format('k = %L::%s', a, typ));
        IF NOT (typ = 'bigint' AND a = '-9223372036854775808') THEN
            PERFORM hm_check_pred(tbl, format('k = %L::%s - %s', a, typ, step));
        END IF;
        IF NOT (typ = 'bigint' AND a = '9223372036854775807') THEN
            PERFORM hm_check_pred(tbl, format('k = %L::%s + %s', a, typ, step));
        END IF;

        CASE i % 5
            WHEN 0 THEN PERFORM hm_check_pred(tbl, format('k < %L::%s', a, typ));
            WHEN 1 THEN PERFORM hm_check_pred(tbl, format('k <= %L::%s', a, typ));
            WHEN 2 THEN PERFORM hm_check_pred(tbl, format('k > %L::%s', b, typ));
            WHEN 3 THEN PERFORM hm_check_pred(tbl, format('k >= %L::%s', b, typ));
            ELSE PERFORM hm_check_pred(tbl, format('k BETWEEN %L::%s AND %L::%s', a, typ, b, typ));
        END CASE;
        cnt := cnt + 1;
    END LOOP;
    RETURN format('%s: ok (%s rounds)', tbl, cnt);
END $$ LANGUAGE plpgsql;

-- One line per index: how much of the table the model describes.
CREATE OR REPLACE FUNCTION hm_summary(idx regclass) RETURNS text AS $$
    SELECT format('%s: %s segments, window avg %s max %s, tail %s pages, %s exceptions, %s folds, %s',
                  idx, segments, round(avg_window::numeric, 1), max_window, tail_pages,
                  exception_entries, folds, pg_size_pretty(index_bytes))
    FROM heapmap_stats(idx);
$$ LANGUAGE sql;
