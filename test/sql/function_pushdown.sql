\unset ECHO
SET client_min_messages = notice;
CREATE TABLE agg_test ( a int, b int, c timestamp, d text );
INSERT INTO agg_test VALUES (1, 1, '2025-10-1 19:33:15', 'first');

DO $do$
DECLARE
  q TEXT;
BEGIN
    FOREACH q IN ARRAY ARRAY[
        -- Aggregates
        'SELECT argMax(a, b) FROM agg_test',
        'SELECT argMax(a, b) FROM agg_test',
        'SELECT argMax(a, c) FROM agg_test',
        'SELECT argMax(a, d) FROM agg_test',
        'SELECT argMax(d, a) FROM agg_test',
        'SELECT argMax(d, b) FROM agg_test',
        'SELECT argMax(d, c) FROM agg_test',

        'SELECT argMin(a, b) FROM agg_test',
        'SELECT argMin(a, c) FROM agg_test',
        'SELECT argMin(a, d) FROM agg_test',
        'SELECT argMin(d, a) FROM agg_test',
        'SELECT argMin(d, b) FROM agg_test',
        'SELECT argMin(d, c) FROM agg_test',

        'SELECT uniq(a) FROM agg_test',
        'SELECT uniq(a, b) FROM agg_test',
        'SELECT uniq(a, b, c) FROM agg_test',
        'SELECT uniq(a, b, c, d) FROM agg_test',

        'SELECT uniqExact(a) FROM agg_test',
        'SELECT uniqExact(a, b) FROM agg_test',
        'SELECT uniqExact(a, b, c) FROM agg_test',
        'SELECT uniqExact(a, b, c, d) FROM agg_test',

        'SELECT uniqCombined(a) FROM agg_test',
        'SELECT uniqCombined(a, b) FROM agg_test',
        'SELECT uniqCombined(a, b, c) FROM agg_test',
        'SELECT uniqCombined(a, b, c, d) FROM agg_test',

        'SELECT uniqCombined64(a) FROM agg_test',
        'SELECT uniqCombined64(a, b) FROM agg_test',
        'SELECT uniqCombined64(a, b, c) FROM agg_test',
        'SELECT uniqCombined64(a, b, c, d) FROM agg_test',

        'SELECT uniqHLL12(a) FROM agg_test',
        'SELECT uniqHLL12(a, b) FROM agg_test',
        'SELECT uniqHLL12(a, b, c) FROM agg_test',
        'SELECT uniqHLL12(a, b, c, d) FROM agg_test',

        'SELECT uniqTheta(a) FROM agg_test',
        'SELECT uniqTheta(a, b) FROM agg_test',
        'SELECT uniqTheta(a, b, c) FROM agg_test',
        'SELECT uniqTheta(a, b, c, d) FROM agg_test',

        -- Functions
        $$ SELECT ch_push_agg_text('hello', 1) $$,
        $$ SELECT ch_push_agg_text('goodbye', true) $$,

        $$ SELECT ch_argmax('x'::text, 'x'::text, 3) $$,
        $$ SELECT ch_argmax(3, 3, true) $$,
        $$ SELECT ch_argmax(true, false, now()) $$,

        $$ SELECT ch_argmin('x'::text, 'x'::text, 3) $$,
        $$ SELECT ch_argmin(3, 3, true) $$,
        $$ SELECT ch_argmin(true, false, now()) $$,

        $$ SELECT dictGet('', '', '{"x": true}'::json) $$,
        $$ SELECT dictGet('a', 'b', ARRAY[1]) $$,

        $$ SELECT quantile(1) $$,
        $$ SELECT quantile('x') $$,
        $$ SELECT quantileExact(42) $$,
        $$ SELECT quantileExact(98.6) $$,

        $$ SELECT toUInt8('x') $$,
        $$ SELECT toUInt8(8) $$,
        $$ SELECT toUInt16('x') $$,
        $$ SELECT toUInt16(16) $$,
        $$ SELECT toUInt32('x') $$,
        $$ SELECT toUInt32(32) $$,
        $$ SELECT toUInt64('x') $$,
        $$ SELECT toUInt64(64) $$

   ] LOOP
        BEGIN
            EXECUTE q;
            RAISE NOTICE '`%`: did not fail', q;
        EXCEPTION WHEN OTHERS OR ASSERT_FAILURE THEN
            RAISE NOTICE '%: % - %', q, SQLSTATE, SQLERRM;
            IF SQLSTATE != 'HV000' THEN
                RAISE EXCEPTION '  Unexpected error code: % - %', SQLSTATE, SQLERRM;
            END IF;
        END;
    END LOOP;
END;
$do$ LANGUAGE plpgsql;
