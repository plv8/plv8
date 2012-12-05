-- window functions
CREATE FUNCTION js_row_number() RETURNS int8 AS $$
  var winobj = plv8.get_window_object();
  return winobj.get_current_position() + 1;
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION __js_rank_up(winobj internal, up_callback internal) RETURNS void AS $$
  var context = winobj.get_partition_local() || {};
  var pos = winobj.get_current_position();
  context.up = false;
  if (!context.rank) {
    context.rank = 1;
  } else {
    if (!winobj.rows_are_peers(pos, pos - 1)) {
      context.up = true;
      if (up_callback) {
        up_callback(context);
      }
    }
  }
  winobj.set_mark_position(pos);
  winobj.set_partition_local(context);
  return context;
$$ LANGUAGE plv8;

CREATE FUNCTION js_rank() RETURNS int8 AS $$
  var winobj = plv8.get_window_object();
  var context = plv8.find_function("__js_rank_up")(winobj, function(context){
    context.rank = winobj.get_current_position() + 1;
  });
  return context.rank;
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_dense_rank() RETURNS int8 AS $$
  var winobj = plv8.get_window_object();
  var context = plv8.find_function("__js_rank_up")(winobj, function(context){
    context.rank++;
  });
  return context.rank;
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_percent_rank() RETURNS float AS $$
  var winobj = plv8.get_window_object();
  var totalrows = winobj.get_partition_row_count();
  if (totalrows <= 1)
    return 0.0;
  var context = plv8.find_function("__js_rank_up")(winobj, function(context){
    context.rank = winobj.get_current_position() + 1;
  });
  return (context.rank - 1) / (totalrows - 1);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_cume_dist() RETURNS float AS $$
  var winobj = plv8.get_window_object();
  var totalrows = winobj.get_partition_row_count();
  var context = plv8.find_function("__js_rank_up")(winobj);
  if (context.up || context.rank == 1) {
    context.rank = winobj.get_current_position() + 1;
    for (var row = context.rank; row < totalrows; row++) {
      if (!winobj.rows_are_peers(row - 1, row)) {
        break;
      }
      context.rank++;
    }
  }
  winobj.set_partition_local(context);
  return context.rank / totalrows;
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_ntile(nbuckets int8) RETURNS int AS $$
  var winobj = plv8.get_window_object();
  var context = winobj.get_partition_local() || {};

  if (!context.ntile) {
    context.rows_per_bucket = 0;
    var total = winobj.get_partition_row_count();
    var nbuckets = winobj.get_func_arg_current(0);
    if (nbuckets === null) {
        return null;
    }
    if (nbuckets <= 0) {
        plv8.elog(ERROR, "argument of ntile must be greater than zero");
    }
    context.ntile = 1;
    context.rows_per_bucket = 0;
    context.boundary = total / nbuckets;
    if (context.boundary <= 0) {
        context.boundary = 1;
    } else {
      context.remainder = total % nbuckets;
      if (context.remainder != 0) {
        context.boundary++;
      }
    }
  }
  context.rows_per_bucket++;
  if (context.boundary < context.rows_per_bucket) {
    if (context.remainder != 0 && context.ntile == context.remainder) {
      context.remainder = 0;
      context.boundary -= 1;
    }
    context.ntile += 1;
    context.rows_per_bucket = 1;
  }
  winobj.set_partition_local(context);
  return context.ntile;
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION __js_lead_lag_common(forward internal, withoffset internal, withdefault internal) RETURNS void AS $$
  var winobj = plv8.get_window_object();
  var offset;
  if (withoffset) {
    offset = winobj.get_func_arg_current(1);
    if (offset === null) {
      return null;
    }
  } else {
    offset = 1;
  }
  var result = winobj.get_func_arg_in_partition(0,
                                                forward ? offset : -offset,
                                                winobj.SEEK_CURRENT,
                                                false);
  if (result === undefined) {
    if (withdefault) {
      result = winobj.get_func_arg_current(2);
    }
  }
  if (result === null) {
    return null;
  }
  return result;
$$ LANGUAGE plv8;

CREATE FUNCTION js_lag(arg anyelement) RETURNS anyelement AS $$
  return plv8.find_function("__js_lead_lag_common")(false, false, false);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_lag(arg anyelement, ofs int) RETURNS anyelement AS $$
  return plv8.find_function("__js_lead_lag_common")(false, true, false);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_lag(arg anyelement, ofs int, deflt anyelement) RETURNS anyelement AS $$
  return plv8.find_function("__js_lead_lag_common")(false, true, true);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_lead(arg anyelement) RETURNS anyelement AS $$
  return plv8.find_function("__js_lead_lag_common")(true, false, false);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_lead(arg anyelement, ofs int) RETURNS anyelement AS $$
  return plv8.find_function("__js_lead_lag_common")(true, true, false);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_lead(arg anyelement, ofs int, deflt anyelement) RETURNS anyelement AS $$
  return plv8.find_function("__js_lead_lag_common")(true, true, true);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_first_value(arg anyelement) RETURNS anyelement AS $$
  var winobj = plv8.get_window_object();
  return winobj.get_func_arg_in_frame(0, 0, winobj.SEEK_HEAD, true);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_last_value(arg anyelement) RETURNS anyelement AS $$
  var winobj = plv8.get_window_object();
  return winobj.get_func_arg_in_frame(0, 0, winobj.SEEK_TAIL, true);
$$ LANGUAGE plv8 WINDOW;

CREATE FUNCTION js_nth_value(arg anyelement, nth int) RETURNS anyelement AS $$
  var winobj = plv8.get_window_object();
  nth = winobj.get_func_arg_current(1);
  if (nth <= 0)
    plv8.elog(ERROR, "argument of nth_value must be greater than zero");
  return winobj.get_func_arg_in_frame(0, nth - 1, winobj.SEEK_HEAD, false);
$$ LANGUAGE plv8 WINDOW;

CREATE TABLE empsalary (
    depname varchar,
    empno bigint,
    salary int,
    enroll_date date
);

INSERT INTO empsalary VALUES
('develop', 10, 5200, '2007-08-01'),
('sales', 1, 5000, '2006-10-01'),
('personnel', 5, 3500, '2007-12-10'),
('sales', 4, 4800, '2007-08-08'),
('personnel', 2, 3900, '2006-12-23'),
('develop', 7, 4200, '2008-01-01'),
('develop', 9, 4500, '2008-01-01'),
('sales', 3, 4800, '2007-08-01'),
('develop', 8, 6000, '2006-10-01'),
('develop', 11, 5200, '2007-08-15');

SELECT row_number() OVER (w), js_row_number() OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT rank() OVER (w), js_rank() OVER (w) FROM empsalary WINDOW w AS (PARTITION BY depname ORDER BY salary);
SELECT dense_rank() OVER (w), js_dense_rank() OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT percent_rank() OVER (w), js_percent_rank() OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT cume_dist() OVER (w), js_cume_dist() OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT ntile(3) OVER (w), js_ntile(3) OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT lag(enroll_date) OVER (w), js_lag(enroll_date) OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT lead(enroll_date) OVER (w), js_lead(enroll_date) OVER (w) FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT first_value(empno) OVER (w ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING),
    js_first_value(empno) OVER (w ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING)
    FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT last_value(empno) OVER (w ROWS BETWEEN 3 PRECEDING AND 1 PRECEDING),
    js_last_value(empno) OVER (w ROWS BETWEEN 3 PRECEDING AND 1 PRECEDING)
    FROM empsalary WINDOW w AS (ORDER BY salary);
SELECT nth_value(empno, 2) OVER (w ROWS BETWEEN 1 FOLLOWING AND 3 FOLLOWING),
    js_nth_value(empno, 2) OVER (w ROWS BETWEEN 1 FOLLOWING AND 3 FOLLOWING)
    FROM empsalary WINDOW w AS (ORDER BY salary);

CREATE FUNCTION bad_alloc(sz text) RETURNS void AS $$
  var winobj = plv8.get_window_object();
  var context = winobj.get_partition_local(sz - 0) || {};
  context.long_text_key_and_value = "blablablablablablablablablablablablablablablabla";
  winobj.set_partition_local(context);
$$ LANGUAGE plv8 WINDOW;

SELECT bad_alloc('5') OVER ();
SELECT bad_alloc('not a number') OVER ();
SELECT bad_alloc('1000') OVER (); -- not so bad

CREATE FUNCTION non_window() RETURNS void AS $$
  var winobj = plv8.get_window_object();
$$ LANGUAGE plv8;

SELECT non_window();
