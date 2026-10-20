/*-------------------------------------------------------------------------
 *
 * plv8_fdw.h
 *    PL/v8 Foreign Data Wrapper header.
 *
 * Copyright (c) 2012-2026, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#ifndef _PLV8_FDW_H_
#define _PLV8_FDW_H_

#include "plv8.h"

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#define String PG_Node_String
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/xact.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#undef String
}

enum class Plv8QualType {
  SIMPLE_OP,
  ARRAY_OP,
  NULL_TEST
};

struct Plv8Qual {
  Plv8QualType type;
  std::string field;
  AttrNumber varattno;
  std::string op;
  Oid opno;
  Node *left_expr;
  Node *right_expr;
  bool is_param;
  bool is_null_test;
  bool null_test_is_not_null;
  Datum const_datum;
  bool const_isnull;
  Oid const_type;
};

struct Plv8ParamCacheEntry {
  uint64 param_hash;
  std::vector<MinimalTuple> tuples;
  size_t memory_used;
};

struct Plv8FdwScanState {
  v8::Isolate *isolate;
  v8::Global<v8::Context> context;
  v8::Global<v8::Object> wrapper_instance;
  v8::Global<v8::Object> iterator;

  TupleDesc tupdesc;
  std::vector<plv8_type> coltypes;
  std::vector<std::string> colname_strs;
  std::vector<v8::Global<v8::String>> colnames;
  std::vector<AttrNumber> projected_attrs;
  std::vector<Plv8Qual> pushed_quals;
  List *fdw_clauses;

  MemoryContext scan_mcxt;
  bool eof_reached;

  // Caching subsystem
  bool cache_enabled;
  size_t cache_size_limit;
  size_t cache_memory_limit_bytes;
  size_t current_cache_bytes;

  // Tier 1: Unparameterized rescan buffer
  bool is_parameterized;
  Tuplestorestate *unparam_tuplestore;
  bool unparam_cache_complete;

  // Tier 2: Parameterized LRU memoizer
  std::unordered_map<uint64, Plv8ParamCacheEntry> param_cache;
  std::list<uint64> param_lru_order;
  uint64 current_param_hash;
  bool replaying_from_cache;
  size_t cached_tuple_cursor;

  // EXPLAIN metrics
  uint64 stats_rows_fetched;
  uint64 stats_cache_hits;
  uint64 stats_cache_misses;
};

struct Plv8FdwModifyState {
  v8::Isolate *isolate;
  v8::Global<v8::Context> context;
  v8::Global<v8::Object> wrapper_instance;
  CmdType operation;
  TupleDesc tupdesc;
  std::vector<plv8_type> coltypes;
  std::vector<std::string> colname_strs;
  std::vector<v8::Global<v8::String>> colnames;
  AttrNumber rowid_attno;
  Oid rowid_type;
  std::string rowid_colname;
  int batch_size;
  std::vector<v8::Global<v8::Object>> batch_rows;
};

struct Plv8FdwDirectModifyState {
  v8::Isolate *isolate;
  v8::Global<v8::Context> context;
  v8::Global<v8::Object> wrapper_instance;
  CmdType operation;
  std::vector<Plv8Qual> quals;
  bool has_executed;
  int affected_rows;
};

extern "C" {
PGDLLEXPORT Datum plv8_fdw_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plv8_fdw_validator(PG_FUNCTION_ARGS);
}

void plv8_fdw_init_context(plv8_context *context);
void plv8_fdw_xact_cb(XactEvent event);

#endif // _PLV8_FDW_H_
