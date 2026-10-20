/*-------------------------------------------------------------------------
 *
 * plv8_fdw.cc
 *    PL/v8 Foreign Data Wrapper implementation.
 *
 * Copyright (c) 2012-2026, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8_fdw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using namespace v8;

/*
 * Static built-in JavaScript library declaring plv8.ForeignDataWrapper,
 * ArrayTableFdw, and MapTableFdw.
 */
static const char *plv8_fdw_bootstrap_js = R"JS(
(function() {
  if (typeof plv8.ForeignDataWrapper !== 'undefined') {
    return;
  }

  class ForeignDataWrapper {
    constructor(options, columns) {
      this.options = options || {};
      this.columns = columns || {};
    }
    get rowidColumn() { return null; }
    get batchSize() { return 1; }
    getRelSize(quals, projectedColumns) { return [1000, 50]; }
    getPathKeys() { return []; }
    canPushdown(qual) { return true; }
    *execute(quals, projectedColumns, sortKeys) {}
    rescan(newQuals) {}
    close() {}
    insert(values) { return values; }
    update(rowid, newValues) { return newValues; }
    delete(rowid) {}
    batchInsert(rows) {
      for (const r of rows) this.insert(r);
    }
    directModify(operation, quals, newValues) { return 0; }
    acquireSample(targetRows) { return []; }
    getEstimatedPages() { return 10; }
    commit() {}
    rollback() {}
    explain(quals, projectedColumns) { return []; }
  }

  plv8.ForeignDataWrapper = ForeignDataWrapper;
  globalThis.ForeignDataWrapper = ForeignDataWrapper;

  class ArrayTableFdw extends ForeignDataWrapper {
    constructor(options, columns) {
      super(options, columns);
      this._rows = [
        { id: 101, username: 'alice',   dept_id: 1, role: 'eng',   salary: 140000 },
        { id: 102, username: 'bob',     dept_id: 1, role: 'eng',   salary: 130000 },
        { id: 103, username: 'charlie', dept_id: 2, role: 'sales', salary: 110000 },
        { id: 104, username: 'david',   dept_id: 2, role: 'sales', salary: 115000 },
        { id: 105, username: 'eva',     dept_id: 3, role: 'lead',  salary: 160000 },
        { id: 106, username: 'frank',   dept_id: 1, role: 'eng',   salary: 125000 }
      ];
    }
    getRelSize(quals, projectedColumns) {
      return [this._rows.length, 64];
    }
    getPathKeys() {
      return [{ column: 'id', ascending: true }];
    }
    canPushdown(qual) {
      return ['=', '<', '>', '<=', '>=', '<>', 'IN', 'IS'].includes(qual.operator);
    }
    *execute(quals, projectedColumns, sortKeys) {
      for (const row of this._rows) {
        let match = true;
        for (const q of quals) {
          const val = row[q.field];
          if (q.operator === 'IS') {
            if (q.value === null && val != null) { match = false; break; }
            if (q.value === 'NOT NULL' && val == null) { match = false; break; }
            continue;
          }
          if (val == null || q.value == null) { match = false; break; }
          if (q.operator === '=' && val != q.value) { match = false; break; }
          if (q.operator === '<>' && val == q.value) { match = false; break; }
          if (q.operator === '<' && !(val < q.value)) { match = false; break; }
          if (q.operator === '>' && !(val > q.value)) { match = false; break; }
          if (q.operator === '<=' && !(val <= q.value)) { match = false; break; }
          if (q.operator === '>=' && !(val >= q.value)) { match = false; break; }
          if (q.operator === 'IN' && Array.isArray(q.value) && !q.value.some(v => v == val)) { match = false; break; }
        }
        if (!match) continue;
        if (!projectedColumns || projectedColumns.length === 0) {
          yield row;
        } else {
          const res = {};
          for (const c of projectedColumns) {
            if (Object.prototype.hasOwnProperty.call(row, c)) res[c] = row[c];
          }
          yield res;
        }
      }
    }
  }

  plv8.ArrayTableFdw = ArrayTableFdw;
  globalThis.ArrayTableFdw = ArrayTableFdw;

  class MapTableFdw extends ForeignDataWrapper {
    constructor(options, columns) {
      super(options, columns);
      this._map = new Map([
        ['sess:1', { key: 'sess:1', user_id: 101, status: 'active',  data: JSON.stringify({theme: 'dark'}) }],
        ['sess:2', { key: 'sess:2', user_id: 102, status: 'expired', data: JSON.stringify({theme: 'light'}) }],
        ['sess:3', { key: 'sess:3', user_id: 103, status: 'active',  data: JSON.stringify({theme: 'system'}) }],
        ['sess:4', { key: 'sess:4', user_id: 104, status: 'active',  data: JSON.stringify({theme: 'dark'}) }]
      ]);
      this._undoLog = [];
    }
    get rowidColumn() { return 'key'; }
    get batchSize() { return 50; }
    getRelSize(quals, projectedColumns) {
      const hasKey = quals && quals.some(q => q.field === 'key' && q.operator === '=');
      return [hasKey ? 1 : this._map.size, 80];
    }
    canPushdown(qual) {
      return ['=', 'IN'].includes(qual.operator);
    }
    *execute(quals, projectedColumns) {
      let targetKey = null;
      let inKeys = null;
      if (quals) {
        for (const q of quals) {
          if (q.field === 'key') {
            if (q.operator === '=') targetKey = q.value;
            if (q.operator === 'IN' && Array.isArray(q.value)) inKeys = new Set(q.value);
          }
        }
      }
      if (targetKey !== null) {
        const rec = this._map.get(String(targetKey));
        if (rec) yield rec;
        return;
      }
      if (inKeys !== null) {
        for (const k of inKeys) {
          const rec = this._map.get(String(k));
          if (rec) yield rec;
        }
        return;
      }
      for (const rec of this._map.values()) {
        yield rec;
      }
    }
    insert(values) {
      if (!values.key) throw new Error("Missing primary key 'key'");
      const prev = this._map.get(values.key);
      this._undoLog.push({ op: 'insert', key: values.key, prev });
      this._map.set(values.key, Object.assign({}, values));
      return values;
    }
    update(rowid, newValues) {
      const existing = this._map.get(rowid);
      if (!existing) throw new Error("Row not found for key: " + rowid);
      this._undoLog.push({ op: 'update', key: rowid, prev: Object.assign({}, existing) });
      const updated = Object.assign({}, existing, newValues, { key: rowid });
      this._map.set(rowid, updated);
      return updated;
    }
    delete(rowid) {
      const existing = this._map.get(rowid);
      if (existing) {
        this._undoLog.push({ op: 'delete', key: rowid, prev: Object.assign({}, existing) });
        this._map.delete(rowid);
      }
    }
    batchInsert(rows) {
      for (const r of rows) this.insert(r);
    }
    directModify(operation, quals, newValues) {
      if (operation === 'DELETE') {
        let count = 0;
        for (const [k, v] of Array.from(this._map.entries())) {
          let match = true;
          if (quals) {
            for (const q of quals) {
              if (q.operator === '=' && String(v[q.field]) !== String(q.value)) { match = false; break; }
            }
          }
          if (match) {
            this.delete(k);
            count++;
          }
        }
        return count;
      }
      return 0;
    }
    commit() {
      this._undoLog = [];
    }
    rollback() {
      while (this._undoLog.length > 0) {
        const e = this._undoLog.pop();
        if (e.op === 'insert') {
          if (e.prev) this._map.set(e.key, e.prev);
          else this._map.delete(e.key);
        } else if (e.op === 'update' || e.op === 'delete') {
          if (e.prev) this._map.set(e.key, e.prev);
        }
      }
    }
  }

  plv8.MapTableFdw = MapTableFdw;
  globalThis.MapTableFdw = MapTableFdw;
})();
)JS";

/*
 * Session-level wrapper instance cache keyed by (user_id, foreigntableid).
 */
struct WrapperCacheKey {
  Oid user_id;
  Oid foreigntableid;
  bool operator==(const WrapperCacheKey &o) const {
    return user_id == o.user_id && foreigntableid == o.foreigntableid;
  }
};

struct WrapperCacheKeyHash {
  std::size_t operator()(const WrapperCacheKey &k) const {
    return std::hash<uint32_t>()(k.user_id) ^ (std::hash<uint32_t>()(k.foreigntableid) << 1);
  }
};

static std::unordered_map<WrapperCacheKey, Global<Object>, WrapperCacheKeyHash> session_wrapper_instances;

void
plv8_fdw_reset(Oid user_id)
{
  for (auto it = session_wrapper_instances.begin(); it != session_wrapper_instances.end(); ) {
    if (!OidIsValid(user_id) || it->first.user_id == user_id) {
      it->second.Reset();
      it = session_wrapper_instances.erase(it);
    } else {
      ++it;
    }
  }
}

void
plv8_fdw_relcache_cb(Datum arg, Oid relid)
{
  if (session_wrapper_instances.empty()) return;
  for (auto it = session_wrapper_instances.begin(); it != session_wrapper_instances.end(); ) {
    if (!OidIsValid(relid) || it->first.foreigntableid == relid) {
      it->second.Reset();
      it = session_wrapper_instances.erase(it);
    } else {
      ++it;
    }
  }
}

static void
plv8_fdw_ensure_bootstrap(Local<Context> context, Isolate *isolate)
{
  Local<String> key = String::NewFromUtf8Literal(isolate, "ForeignDataWrapper", NewStringType::kInternalized);
  Local<Object> global = context->Global();
  bool has_base = false;
  if (global->Has(context, key).To(&has_base) && has_base) {
    return;
  }

  TryCatch try_catch(isolate);
  Local<String> src = String::NewFromUtf8(isolate, plv8_fdw_bootstrap_js, NewStringType::kNormal).ToLocalChecked();
  Local<Script> script;
  if (!Script::Compile(context, src).ToLocal(&script)) {
    if (try_catch.HasCaught()) throw js_error(try_catch);
    throw js_error("failed to compile plv8_fdw bootstrap script");
  }
  Local<Value> res;
  if (!script->Run(context).ToLocal(&res)) {
    if (try_catch.HasCaught()) throw js_error(try_catch);
    throw js_error("failed to execute plv8_fdw bootstrap script");
  }
}

void
plv8_fdw_init_context(plv8_context *context)
{
  if (!context || !context->isolate) return;
  Isolate *isolate = context->isolate;
  Isolate::Scope iso_scope(isolate);
  HandleScope handle_scope(isolate);
  Local<Context> ctx = context->localContext();
  Context::Scope ctx_scope(ctx);

  plv8_fdw_ensure_bootstrap(ctx, isolate);
}

void
plv8_fdw_xact_cb(XactEvent event)
{
  if (session_wrapper_instances.empty()) return;

  const char *method_name = (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE)
                                ? "commit"
                                : (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT ? "rollback" : nullptr);
  if (!method_name) return;

  try {
    plv8_context *context = GetPlv8Context();
    if (!context || !context->isolate) return;
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope handle_scope(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    Local<String> m_str = String::NewFromUtf8(isolate, method_name, NewStringType::kInternalized).ToLocalChecked();

    for (auto &pair : session_wrapper_instances) {
      if (pair.first.user_id != context->user_id) continue;
      if (pair.second.IsEmpty()) continue;

      Local<Object> inst = pair.second.Get(isolate);
      Local<Value> fn_val;
      if (inst->Get(ctx, m_str).ToLocal(&fn_val) && fn_val->IsFunction()) {
        Local<Function> fn = Local<Function>::Cast(fn_val);
        TryCatch tc(isolate);
        (void) fn->Call(ctx, inst, 0, nullptr);
      }
    }
  }
  catch (...) {}
}

/*
 * Extracts table and server options into a key-value std::unordered_map.
 */
static std::unordered_map<std::string, std::string>
plv8_fdw_get_options(Oid foreigntableid)
{
  std::unordered_map<std::string, std::string> opts;
  ForeignTable *table = GetForeignTable(foreigntableid);
  ForeignServer *server = GetForeignServer(table->serverid);

  List *options = NIL;
  options = list_concat(options, server->options);
  options = list_concat(options, table->options);

  ListCell *lc;
  foreach (lc, options) {
    DefElem *def = (DefElem *) lfirst(lc);
    opts[def->defname] = defGetString(def);
  }
  return opts;
}

/*
 * Instantiates or retrieves cached JavaScript wrapper object for a foreign table.
 */
static Local<Object>
plv8_fdw_get_wrapper_instance(plv8_context *context, Oid foreigntableid, const std::unordered_map<std::string, std::string> &opts)
{
  Isolate *isolate = context->isolate;
  Local<Context> ctx = context->localContext();
  Oid user_id = context->user_id;

  WrapperCacheKey key{user_id, foreigntableid};
  auto it = session_wrapper_instances.find(key);
  if (it != session_wrapper_instances.end() && !it->second.IsEmpty()) {
    return it->second.Get(isolate);
  }

  plv8_fdw_ensure_bootstrap(ctx, isolate);

  std::string wrapper_name = "ArrayTableFdw";
  auto opt_it = opts.find("wrapper");
  if (opt_it != opts.end() && !opt_it->second.empty()) {
    wrapper_name = opt_it->second;
  }

  Local<Object> global = ctx->Global();
  Local<Value> constructor_val;
  Local<String> w_str = String::NewFromUtf8(isolate, wrapper_name.c_str(), NewStringType::kNormal).ToLocalChecked();

  if (!global->Get(ctx, w_str).ToLocal(&constructor_val) || !constructor_val->IsFunction()) {
    Local<String> plv8_str = String::NewFromUtf8Literal(isolate, "plv8", NewStringType::kInternalized);
    Local<Value> plv8_val;
    if (global->Get(ctx, plv8_str).ToLocal(&plv8_val) && plv8_val->IsObject()) {
      Local<Object> plv8_obj = plv8_val.As<Object>();
      (void) plv8_obj->Get(ctx, w_str).ToLocal(&constructor_val);
    }
  }

  if (constructor_val.IsEmpty() || !constructor_val->IsFunction()) {
    ereport(ERROR,
            (errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
             errmsg("plv8_fdw: could not find JavaScript wrapper constructor \"%s\"", wrapper_name.c_str())));
  }

  Local<Function> constructor = constructor_val.As<Function>();

  // Build options object
  Local<Object> options_obj = Object::New(isolate);
  for (const auto &pair : opts) {
    Local<String> k = String::NewFromUtf8(isolate, pair.first.c_str(), NewStringType::kNormal).ToLocalChecked();
    Local<String> v = String::NewFromUtf8(isolate, pair.second.c_str(), NewStringType::kNormal).ToLocalChecked();
    (void) options_obj->Set(ctx, k, v);
  }

  // Build columns object with safe RAII TableCloseGuard
  Local<Object> columns_obj = Object::New(isolate);
  {
    Relation rel = table_open(foreigntableid, AccessShareLock);
    TableCloseGuard guard(rel, AccessShareLock);
    TupleDesc tupdesc = RelationGetDescr(rel);

    for (int i = 0; i < tupdesc->natts; i++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (attr->attisdropped) continue;

      Local<Object> col_info = Object::New(isolate);
      char *cname = NameStr(attr->attname);
      Local<String> col_name_str = String::NewFromUtf8(isolate, cname, NewStringType::kNormal).ToLocalChecked();
      (void) col_info->Set(ctx, String::NewFromUtf8Literal(isolate, "name"), col_name_str);
      (void) col_info->Set(ctx, String::NewFromUtf8Literal(isolate, "typeOid"), v8::Integer::New(isolate, (int32_t) attr->atttypid));

      char *typname = format_type_be(attr->atttypid);
      (void) col_info->Set(ctx, String::NewFromUtf8Literal(isolate, "typeName"),
                           String::NewFromUtf8(isolate, typname, NewStringType::kNormal).ToLocalChecked());
      pfree(typname);

      char category;
      bool ispreferred;
      get_type_category_preferred(attr->atttypid, &category, &ispreferred);
      char cat_str[2] = {category, '\0'};
      (void) col_info->Set(ctx, String::NewFromUtf8Literal(isolate, "category"),
                           String::NewFromUtf8(isolate, cat_str, NewStringType::kNormal).ToLocalChecked());

      (void) columns_obj->Set(ctx, col_name_str, col_info);
    }
  }

  Local<Value> ctor_args[2] = {options_obj, columns_obj};
  TryCatch try_catch(isolate);
  Local<Value> instance_val;
  if (!constructor->CallAsConstructor(ctx, 2, ctor_args).ToLocal(&instance_val) || !instance_val->IsObject()) {
    if (try_catch.HasCaught()) throw js_error(try_catch);
    throw js_error("failed to instantiate plv8_fdw wrapper");
  }

  Local<Object> instance = instance_val.As<Object>();
  session_wrapper_instances[key].Reset(isolate, instance);
  return instance;
}

/*
 * Converts a PostgreSQL operator expression into a Plv8Qual.
 */
static bool
plv8_fdw_extract_qual(Node *clause, Index varno, TupleDesc tupdesc, Plv8Qual &out_qual)
{
  out_qual.expr_state = nullptr;

  if (IsA(clause, OpExpr)) {
    OpExpr *op = (OpExpr *) clause;
    if (list_length(op->args) != 2) return false;

    Node *left = (Node *) linitial(op->args);
    Node *right = (Node *) lsecond(op->args);
    bool inverted = false;

    if (IsA(left, Var) && ((Var *) left)->varno == varno) {
      // Normal: col op const/param
    } else if (IsA(right, Var) && ((Var *) right)->varno == varno) {
      // Inverted: const/param op col -> commute
      std::swap(left, right);
      inverted = true;
    } else {
      return false;
    }

    Var *var = (Var *) left;
    if (var->varattno <= 0 || var->varattno > tupdesc->natts) return false;

    Oid opno = op->opno;
    if (inverted) {
      opno = get_commutator(opno);
      if (!OidIsValid(opno)) return false;
    }

    char *opname = get_opname(opno);
    if (!opname) return false;

    out_qual.type = Plv8QualType::SIMPLE_OP;
    out_qual.field = NameStr(TupleDescAttr(tupdesc, var->varattno - 1)->attname);
    out_qual.varattno = var->varattno;
    out_qual.op = opname;
    out_qual.opno = opno;
    out_qual.left_expr = left;
    out_qual.right_expr = right;
    out_qual.is_null_test = false;

    if (IsA(right, Const)) {
      Const *c = (Const *) right;
      out_qual.is_param = false;
      out_qual.const_datum = c->constvalue;
      out_qual.const_isnull = c->constisnull;
      out_qual.const_type = c->consttype;
    } else {
      out_qual.is_param = true;
    }
    return true;
  }

  if (IsA(clause, ScalarArrayOpExpr)) {
    ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
    if (list_length(saop->args) != 2 || !saop->useOr) return false;

    Node *left = (Node *) linitial(saop->args);
    Node *right = (Node *) lsecond(saop->args);

    if (!IsA(left, Var) || ((Var *) left)->varno != varno) return false;

    Var *var = (Var *) left;
    if (var->varattno <= 0 || var->varattno > tupdesc->natts) return false;

    char *opname = get_opname(saop->opno);
    if (!opname || strcmp(opname, "=") != 0) return false;

    out_qual.type = Plv8QualType::ARRAY_OP;
    out_qual.field = NameStr(TupleDescAttr(tupdesc, var->varattno - 1)->attname);
    out_qual.varattno = var->varattno;
    out_qual.op = "IN";
    out_qual.opno = saop->opno;
    out_qual.left_expr = left;
    out_qual.right_expr = right;
    out_qual.is_null_test = false;

    if (IsA(right, Const)) {
      Const *c = (Const *) right;
      out_qual.is_param = false;
      out_qual.const_datum = c->constvalue;
      out_qual.const_isnull = c->constisnull;
      out_qual.const_type = c->consttype;
    } else {
      out_qual.is_param = true;
    }
    return true;
  }

  if (IsA(clause, NullTest)) {
    NullTest *nt = (NullTest *) clause;
    if (!IsA(nt->arg, Var) || ((Var *) nt->arg)->varno != varno) return false;

    Var *var = (Var *) nt->arg;
    if (var->varattno <= 0 || var->varattno > tupdesc->natts) return false;

    out_qual.type = Plv8QualType::NULL_TEST;
    out_qual.field = NameStr(TupleDescAttr(tupdesc, var->varattno - 1)->attname);
    out_qual.varattno = var->varattno;
    out_qual.op = "IS";
    out_qual.opno = InvalidOid;
    out_qual.left_expr = (Node *) var;
    out_qual.right_expr = nullptr;
    out_qual.is_param = false;
    out_qual.is_null_test = true;
    out_qual.null_test_is_not_null = (nt->nulltesttype == IS_NOT_NULL);
    out_qual.const_isnull = true;
    return true;
  }

  return false;
}

/*
 * Converts Plv8Qual to a JavaScript Qual object.
 */
static Local<Object>
plv8_fdw_qual_to_js(Local<Context> ctx, Isolate *isolate, const Plv8Qual &q, ExprContext *econtext = nullptr)
{
  Local<Object> obj = Object::New(isolate);
  (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "field"),
                  String::NewFromUtf8(isolate, q.field.c_str(), NewStringType::kNormal).ToLocalChecked());
  (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "operator"),
                  String::NewFromUtf8(isolate, q.op.c_str(), NewStringType::kNormal).ToLocalChecked());
  (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "isArray"),
                  v8::Boolean::New(isolate, q.type == Plv8QualType::ARRAY_OP));
  (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "isParam"),
                  v8::Boolean::New(isolate, q.is_param));

  if (q.is_null_test) {
    if (q.null_test_is_not_null) {
      (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "value"),
                      String::NewFromUtf8Literal(isolate, "NOT NULL"));
    } else {
      (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "value"), v8::Null(isolate));
    }
    return obj;
  }

  Datum val_datum = (Datum) 0;
  bool val_isnull = false;
  Oid val_type = InvalidOid;

  if (q.is_param && econtext && q.right_expr) {
    if (q.expr_state) {
      val_datum = ExecEvalExpr(q.expr_state, econtext, &val_isnull);
    } else {
      ExprState *estate_expr = ExecInitExpr((Expr *) q.right_expr, nullptr);
      val_datum = ExecEvalExpr(estate_expr, econtext, &val_isnull);
    }
    val_type = exprType(q.right_expr);
  } else {
    val_datum = q.const_datum;
    val_isnull = q.const_isnull;
    val_type = q.const_type;
  }

  if (val_isnull) {
    (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "value"), v8::Null(isolate));
  } else if (OidIsValid(val_type)) {
    plv8_type pt;
    memset(&pt, 0, sizeof(pt));
    plv8_fill_type(&pt, val_type);
    Local<Value> jval = ToValue(val_datum, false, &pt);
    (void) obj->Set(ctx, String::NewFromUtf8Literal(isolate, "value"), jval);
  }
  return obj;
}

/*
 * FDW Planning Callback: GetForeignRelSize
 */
static void
plv8_fdw_get_foreign_rel_size(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
  try {
    plv8_context *context = GetPlv8Context();
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
    Local<Object> wrapper = plv8_fdw_get_wrapper_instance(context, foreigntableid, opts);

    std::vector<Plv8Qual> candidate_quals;
    {
      Relation rel = table_open(foreigntableid, AccessShareLock);
      TableCloseGuard guard(rel, AccessShareLock);
      TupleDesc tupdesc = RelationGetDescr(rel);

      ListCell *lc;
      foreach (lc, baserel->baserestrictinfo) {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        Plv8Qual q;
        if (plv8_fdw_extract_qual((Node *) ri->clause, baserel->relid, tupdesc, q)) {
          candidate_quals.push_back(q);
        }
      }
    }

    Local<Array> quals_arr = Array::New(isolate, (int) candidate_quals.size());
    for (size_t i = 0; i < candidate_quals.size(); i++) {
      (void) quals_arr->Set(ctx, (uint32_t) i, plv8_fdw_qual_to_js(ctx, isolate, candidate_quals[i]));
    }

    Local<Array> proj_arr = Array::New(isolate);
    Local<Value> fn_val;
    double estimated_rows = 1000.0;
    int estimated_width = 50;

    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "getRelSize")).ToLocal(&fn_val) && fn_val->IsFunction()) {
      Local<Function> fn = fn_val.As<Function>();
      Local<Value> args[2] = {quals_arr, proj_arr};
      TryCatch tc(isolate);
      Local<Value> res_val;
      if (fn->Call(ctx, wrapper, 2, args).ToLocal(&res_val) && res_val->IsArray()) {
        Local<Array> res_arr = res_val.As<Array>();
        if (res_arr->Length() >= 1) {
          Local<Value> r0 = res_arr->Get(ctx, 0).ToLocalChecked();
          if (r0->IsNumber()) estimated_rows = r0->NumberValue(ctx).FromMaybe(1000.0);
        }
        if (res_arr->Length() >= 2) {
          Local<Value> r1 = res_arr->Get(ctx, 1).ToLocalChecked();
          if (r1->IsInt32()) estimated_width = r1->Int32Value(ctx).FromMaybe(50);
        }
      }
    }

    baserel->rows = clamp_row_est(estimated_rows);
    baserel->reltarget->width = estimated_width;
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
}

/*
 * FDW Planning Callback: GetForeignPaths
 */
static void
plv8_fdw_get_foreign_paths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
  try {
    plv8_context *context = GetPlv8Context();
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
    Local<Object> wrapper = plv8_fdw_get_wrapper_instance(context, foreigntableid, opts);

    Cost startup_cost = 10.0;
    Cost total_cost = startup_cost + baserel->rows * 0.2;

    // Check sorted pathkeys
    List *pathkeys = NIL;
    Local<Value> pk_val;
    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "getPathKeys")).ToLocal(&pk_val) && pk_val->IsFunction()) {
      Local<Function> pk_fn = pk_val.As<Function>();
      TryCatch tc(isolate);
      Local<Value> res;
      if (pk_fn->Call(ctx, wrapper, 0, nullptr).ToLocal(&res) && res->IsArray()) {
        Local<Array> arr = res.As<Array>();
        if (arr->Length() > 0) {
          Local<Value> item = arr->Get(ctx, 0).ToLocalChecked();
          if (item->IsObject()) {
            Local<Object> obj = item.As<Object>();
            Local<Value> col_val = obj->Get(ctx, String::NewFromUtf8Literal(isolate, "column")).ToLocalChecked();
            if (col_val->IsString()) {
              CString col_cstr(col_val);
              Relation rel = table_open(foreigntableid, AccessShareLock);
              TableCloseGuard guard(rel, AccessShareLock);
              TupleDesc tupdesc = RelationGetDescr(rel);
              for (int a = 0; a < tupdesc->natts; a++) {
                Form_pg_attribute attr = TupleDescAttr(tupdesc, a);
                if (!attr->attisdropped && strcmp(NameStr(attr->attname), col_cstr.str()) == 0) {
                  Var *var = makeVar(baserel->relid, attr->attnum, attr->atttypid, attr->atttypmod, attr->attcollation, 0);
                  Oid ltOpr = InvalidOid;
                  get_sort_group_operators(attr->atttypid, true, false, false, &ltOpr, NULL, NULL, NULL);
                  if (OidIsValid(ltOpr)) {
                    pathkeys = build_expression_pathkey(root, (Expr *) var, ltOpr, NULL, true);
                  }
                  break;
                }
              }
            }
          }
        }
      }
    }

#if PG_VERSION_NUM >= 180000
    ForeignPath *path = create_foreignscan_path(
        root, baserel,
        NULL,
        baserel->rows,
        0,                 /* disabled_nodes */
        startup_cost, total_cost,
        pathkeys,
        NULL,
        NULL,
        NIL,
        NIL);
#else
    ForeignPath *path = create_foreignscan_path(
        root, baserel,
        NULL,
        baserel->rows,
        startup_cost, total_cost,
        pathkeys,
        NULL,
        NULL,
        NIL,
        NIL);
#endif
    add_path(baserel, (Path *) path);

    // Generate parameterized paths for inner nested loop joins if join clauses exist
    if (baserel->joininfo != NIL) {
      Relation rel = table_open(foreigntableid, AccessShareLock);
      TableCloseGuard guard(rel, AccessShareLock);
      TupleDesc tupdesc = RelationGetDescr(rel);

      ListCell *lc;
      foreach (lc, baserel->joininfo) {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
        if (!join_clause_is_movable_to(rinfo, baserel))
          continue;

        Plv8Qual q;
        if (!plv8_fdw_extract_qual((Node *) rinfo->clause, baserel->relid, tupdesc, q))
          continue;

        Relids required_outer = bms_union(rinfo->clause_relids, baserel->lateral_relids);
        required_outer = bms_del_member(required_outer, baserel->relid);
        if (bms_is_empty(required_outer))
          continue;

        ParamPathInfo *param_info = get_baserel_parampathinfo(root, baserel, required_outer);
        if (!param_info)
          continue;

        Cost param_startup_cost = 1.0;
        Cost param_total_cost = param_startup_cost + param_info->ppi_rows * 0.1;

#if PG_VERSION_NUM >= 180000
        ForeignPath *ppath = create_foreignscan_path(
            root, baserel,
            NULL,
            param_info->ppi_rows,
            0,                 /* disabled_nodes */
            param_startup_cost, param_total_cost,
            NIL,
            param_info->ppi_req_outer,
            NULL,
            NIL,
            NIL);
#else
        ForeignPath *ppath = create_foreignscan_path(
            root, baserel,
            NULL,
            param_info->ppi_rows,
            param_startup_cost, param_total_cost,
            NIL,
            param_info->ppi_req_outer,
            NULL,
            NIL,
            NIL);
#endif
        add_path(baserel, (Path *) ppath);
      }
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
}

/*
 * FDW Planning Callback: GetForeignPlan
 */
static ForeignScan *
plv8_fdw_get_foreign_plan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
                          ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)
{
  List *fdw_clauses = NIL;
  List *local_quals = NIL;

  try {
    plv8_context *context = GetPlv8Context();
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
    Local<Object> wrapper = plv8_fdw_get_wrapper_instance(context, foreigntableid, opts);

    Relation rel = table_open(foreigntableid, AccessShareLock);
    TableCloseGuard guard(rel, AccessShareLock);
    TupleDesc tupdesc = RelationGetDescr(rel);

    Local<Value> can_push_fn_val;
    bool has_can_push = (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "canPushdown")).ToLocal(&can_push_fn_val) &&
                         can_push_fn_val->IsFunction());

    ListCell *lc;
    foreach (lc, scan_clauses) {
      RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
      Plv8Qual q;
      bool pushable = false;

      if (plv8_fdw_extract_qual((Node *) ri->clause, baserel->relid, tupdesc, q)) {
        if (has_can_push) {
          Local<Function> can_push_fn = can_push_fn_val.As<Function>();
          Local<Value> q_obj = plv8_fdw_qual_to_js(ctx, isolate, q);
          Local<Value> res_val;
          TryCatch tc(isolate);
          if (can_push_fn->Call(ctx, wrapper, 1, &q_obj).ToLocal(&res_val) && res_val->IsBoolean()) {
            pushable = res_val->BooleanValue(isolate);
          }
        } else {
          pushable = true;
        }
      }

      if (pushable) {
        fdw_clauses = lappend(fdw_clauses, ri);
      } else {
        local_quals = lappend(local_quals, ri);
      }
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }

  scan_clauses = extract_actual_clauses(local_quals, false);
  fdw_clauses = extract_actual_clauses(fdw_clauses, false);

  return make_foreignscan(
      tlist,
      scan_clauses,
      baserel->relid,
      fdw_clauses,
      NIL,
      NIL,
      NIL,
      outer_plan);
}

/*
 * Helper: populates a virtual slot from a JavaScript record object.
 */
static void
plv8_fdw_populate_slot(Local<Value> row_val, TupleTableSlot *slot, Plv8FdwScanState *festate)
{
  Isolate *isolate = festate->isolate;
  Local<Context> ctx = festate->context.Get(isolate);

  if (!row_val->IsObject()) return;
  Local<Object> row_obj = row_val.As<Object>();

  for (size_t i = 0; i < festate->coltypes.size(); i++) {
    AttrNumber attno = (AttrNumber) (i + 1);

    if (!festate->projected_attrs.empty()) {
      bool found = false;
      for (AttrNumber pa : festate->projected_attrs) {
        if (pa == attno) { found = true; break; }
      }
      if (!found) {
        slot->tts_isnull[i] = true;
        slot->tts_values[i] = (Datum) 0;
        continue;
      }
    }

    Local<String> cname = festate->colnames[i].Get(isolate);
    bool has_prop = false;
    if (row_obj->Has(ctx, cname).To(&has_prop) && has_prop) {
      Local<Value> val;
      if (row_obj->Get(ctx, cname).ToLocal(&val) && !val->IsNull() && !val->IsUndefined()) {
        bool isnull = false;
        Datum d = ToDatum(val, &isnull, &festate->coltypes[i]);
        slot->tts_values[i] = d;
        slot->tts_isnull[i] = isnull;
      } else {
        slot->tts_values[i] = (Datum) 0;
        slot->tts_isnull[i] = true;
      }
    } else {
      slot->tts_values[i] = (Datum) 0;
      slot->tts_isnull[i] = true;
    }
  }
}

/*
 * Evaluates runtime parameter hash for LRU caching.
 */
static uint64
plv8_fdw_eval_param_hash(Plv8FdwScanState *festate, ForeignScanState *node)
{
  uint64 hash = 14695981039346656037ULL;
  ExprContext *econtext = node->ss.ps.ps_ExprContext;

  for (const auto &q : festate->pushed_quals) {
    if (!q.is_param || !q.right_expr) continue;
    bool isnull = false;
    Datum val = (Datum) 0;
    if (q.expr_state) {
      val = ExecEvalExpr(q.expr_state, econtext, &isnull);
    } else {
      ExprState *estate_expr = ExecInitExpr((Expr *) q.right_expr, nullptr);
      val = ExecEvalExpr(estate_expr, econtext, &isnull);
    }

    hash ^= (uint64) q.varattno;
    hash *= 1099511628211ULL;
    hash ^= (uint64) (isnull ? 1 : 0);
    hash *= 1099511628211ULL;
    if (!isnull) {
      hash ^= (uint64) val;
      hash *= 1099511628211ULL;
    }
  }
  return hash;
}

/*
 * Invokes wrapper.execute() in JavaScript and prepares generator iterator.
 */
static void
plv8_fdw_invoke_execute(Plv8FdwScanState *festate, ForeignScanState *node)
{
  Isolate *isolate = festate->isolate;
  Local<Context> ctx = festate->context.Get(isolate);
  Local<Object> wrapper = festate->wrapper_instance.Get(isolate);
  ExprContext *econtext = node->ss.ps.ps_ExprContext;

  Local<Array> quals_arr = Array::New(isolate, (int) festate->pushed_quals.size());
  for (size_t i = 0; i < festate->pushed_quals.size(); i++) {
    (void) quals_arr->Set(ctx, (uint32_t) i, plv8_fdw_qual_to_js(ctx, isolate, festate->pushed_quals[i], econtext));
  }

  Local<Array> proj_arr = Array::New(isolate, (int) festate->projected_attrs.size());
  for (size_t i = 0; i < festate->projected_attrs.size(); i++) {
    AttrNumber a = festate->projected_attrs[i];
    if (a > 0 && a <= (int) festate->colname_strs.size()) {
      Local<String> col_s = String::NewFromUtf8(isolate, festate->colname_strs[a - 1].c_str(), NewStringType::kNormal).ToLocalChecked();
      (void) proj_arr->Set(ctx, (uint32_t) i, col_s);
    }
  }

  Local<Value> exec_val;
  if (!wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "execute")).ToLocal(&exec_val) || !exec_val->IsFunction()) {
    throw js_error("wrapper.execute is not a function");
  }

  Local<Function> exec_fn = exec_val.As<Function>();
  Local<Value> args[2] = {quals_arr, proj_arr};
  TryCatch try_catch(isolate);
  Local<Value> iter_val;
  if (!exec_fn->Call(ctx, wrapper, 2, args).ToLocal(&iter_val) || !iter_val->IsObject()) {
    if (try_catch.HasCaught()) throw js_error(try_catch);
    throw js_error("wrapper.execute did not return an iterable or generator object");
  }

  Local<Object> iter_obj = iter_val.As<Object>();
  Local<String> next_str = String::NewFromUtf8Literal(isolate, "next", NewStringType::kInternalized);

  if (!iter_obj->Has(ctx, next_str).FromMaybe(false)) {
    Local<Value> sym_iter;
    Local<Symbol> iterator_sym = Symbol::GetIterator(isolate);
    if (iter_obj->Get(ctx, iterator_sym).ToLocal(&sym_iter) && sym_iter->IsFunction()) {
      Local<Function> get_iter_fn = sym_iter.As<Function>();
      iter_val = get_iter_fn->Call(ctx, iter_obj, 0, nullptr).ToLocalChecked();
      if (iter_val->IsObject()) iter_obj = iter_val.As<Object>();
    }
  }

  festate->iterator.Reset(isolate, iter_obj);
}

static void
plv8_fdw_scan_cleanup_callback(void *arg)
{
  Plv8FdwScanState *festate = (Plv8FdwScanState *) arg;
  if (!festate) return;

  Isolate *isolate = festate->isolate;
  if (isolate) {
    Isolate::Scope iso_scope(isolate);
    festate->iterator.Reset();
    festate->wrapper_instance.Reset();
    festate->context.Reset();
    for (auto &cn : festate->colnames) {
      cn.Reset();
    }
    festate->colnames.clear();
    festate->~Plv8FdwScanState();
  } else {
    festate->~Plv8FdwScanState();
  }
}

/*
 * FDW Scan Callback: BeginForeignScan
 */
static void
plv8_fdw_begin_foreign_scan(ForeignScanState *node, int eflags)
{
  if (eflags & EXEC_FLAG_EXPLAIN_ONLY) return;

  ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
  Oid foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);

  try {
    plv8_context *context = GetPlv8Context();
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
    Local<Object> wrapper = plv8_fdw_get_wrapper_instance(context, foreigntableid, opts);

    MemoryContext query_cxt = node->ss.ps.state->es_query_cxt;
    MemoryContext scan_mcxt = AllocSetContextCreate(
        query_cxt,
        "plv8_fdw scan context",
        ALLOCSET_DEFAULT_SIZES);

    MemoryContext oldcxt = MemoryContextSwitchTo(scan_mcxt);

    Plv8FdwScanState *festate = (Plv8FdwScanState *) palloc0(sizeof(Plv8FdwScanState));
    new (festate) Plv8FdwScanState();
    festate->isolate = isolate;
    festate->context.Reset(isolate, ctx);
    festate->wrapper_instance.Reset(isolate, wrapper);
    festate->scan_mcxt = scan_mcxt;
    festate->eof_reached = false;
    festate->fdw_clauses = plan->fdw_exprs;

    // Register RAII cleanup callback to guarantee handle disposal on query abort or normal exit
    festate->cleanup_cb.func = plv8_fdw_scan_cleanup_callback;
    festate->cleanup_cb.arg = festate;
    MemoryContextRegisterResetCallback(scan_mcxt, &festate->cleanup_cb);

    // Cache configurations
    festate->cache_enabled = true;
    auto opt_cache = opts.find("cache_results");
    if (opt_cache != opts.end() && (opt_cache->second == "false" || opt_cache->second == "off")) {
      festate->cache_enabled = false;
    }
    festate->cache_size_limit = 1000;
    auto opt_size = opts.find("cache_size");
    if (opt_size != opts.end()) {
      festate->cache_size_limit = (size_t) std::max(1, std::atoi(opt_size->second.c_str()));
    }
    festate->cache_memory_limit_bytes = 16 * 1024 * 1024;
    festate->current_cache_bytes = 0;

    TupleDesc tupdesc = RelationGetDescr(node->ss.ss_currentRelation);
    festate->tupdesc = tupdesc;
    festate->coltypes.resize(tupdesc->natts);
    festate->colnames.resize(tupdesc->natts);
    festate->colname_strs.resize(tupdesc->natts);

    for (int i = 0; i < tupdesc->natts; i++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (attr->attisdropped) continue;
      memset(&festate->coltypes[i], 0, sizeof(plv8_type));
      plv8_fill_type(&festate->coltypes[i], attr->atttypid, scan_mcxt);
      char *cn = NameStr(attr->attname);
      festate->colname_strs[i] = cn;
      festate->colnames[i].Reset(isolate, String::NewFromUtf8(isolate, cn, NewStringType::kNormal).ToLocalChecked());
    }

    // Determine projected columns from targetlist safely via Bitmapset
    Bitmapset *attrs_used = NULL;
    ListCell *lc;
    foreach (lc, node->ss.ps.plan->targetlist) {
      TargetEntry *tle = lfirst_node(TargetEntry, lc);
      pull_varattnos((Node *) tle->expr, plan->scan.scanrelid, &attrs_used);
    }
    int col = -1;
    while ((col = bms_next_member(attrs_used, col)) >= 0) {
      AttrNumber attno = col + FirstLowInvalidHeapAttributeNumber;
      if (attno > 0) {
        festate->projected_attrs.push_back(attno);
      }
    }
    bms_free(attrs_used);

    // Extract pushed quals and initialize ExprState once
    festate->is_parameterized = false;
    foreach (lc, plan->fdw_exprs) {
      Node *clause = (Node *) lfirst(lc);
      Plv8Qual q;
      if (plv8_fdw_extract_qual(clause, plan->scan.scanrelid, tupdesc, q)) {
        if (q.is_param) {
          festate->is_parameterized = true;
          if (q.right_expr) {
            q.expr_state = ExecInitExpr((Expr *) q.right_expr, (PlanState *) node);
          }
        }
        festate->pushed_quals.push_back(q);
      }
    }

    if (!festate->is_parameterized && festate->cache_enabled) {
      festate->unparam_tuplestore = tuplestore_begin_heap(true, false, work_mem);
      festate->unparam_cache_complete = false;
    }

    plv8_fdw_invoke_execute(festate, node);

    node->fdw_state = (void *) festate;
    MemoryContextSwitchTo(oldcxt);
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
}

/*
 * FDW Scan Callback: IterateForeignScan
 */
static TupleTableSlot *
plv8_fdw_iterate_foreign_scan(ForeignScanState *node)
{
  Plv8FdwScanState *festate = (Plv8FdwScanState *) node->fdw_state;
  TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
  ExecClearTuple(slot);

  if (!festate || festate->eof_reached) return slot;

  // Tier 1 Replay: Serving unparameterized scan from Tuplestore (safe on any slot type)
  if (!festate->is_parameterized && festate->unparam_cache_complete && festate->unparam_tuplestore) {
    if (tuplestore_gettupleslot_force(festate->unparam_tuplestore, true, false, slot)) {
      festate->stats_rows_fetched++;
      return slot;
    }
    festate->eof_reached = true;
    return slot;
  }

  // Tier 2 Replay: Serving parameterized scan from LRU cache (force-store on virtual slot)
  if (festate->replaying_from_cache) {
    auto it = festate->param_cache.find(festate->current_param_hash);
    if (it != festate->param_cache.end()) {
      auto &entry = it->second;
      if (festate->cached_tuple_cursor < entry.tuples.size()) {
        MinimalTuple mt = entry.tuples[festate->cached_tuple_cursor++];
        ExecForceStoreMinimalTuple(mt, slot, false);
        festate->stats_rows_fetched++;
        return slot;
      }
    }
    festate->eof_reached = true;
    return slot;
  }

  CHECK_FOR_INTERRUPTS();

  try {
    Isolate *isolate = festate->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = festate->context.Get(isolate);
    Context::Scope ctx_scope(ctx);

    if (festate->iterator.IsEmpty()) {
      festate->eof_reached = true;
      return slot;
    }

    Local<Object> iterator = festate->iterator.Get(isolate);
    Local<String> next_str = String::NewFromUtf8Literal(isolate, "next", NewStringType::kInternalized);
    Local<Value> next_val;
    if (!iterator->Get(ctx, next_str).ToLocal(&next_val) || !next_val->IsFunction()) {
      festate->eof_reached = true;
      return slot;
    }

    Local<Function> next_fn = next_val.As<Function>();
    TryCatch try_catch(isolate);
    Local<Value> step_val;
    if (!next_fn->Call(ctx, iterator, 0, nullptr).ToLocal(&step_val) || !step_val->IsObject()) {
      if (try_catch.HasCaught()) throw js_error(try_catch);
      festate->eof_reached = true;
      return slot;
    }

    Local<Object> step_obj = step_val.As<Object>();
    Local<String> done_str = String::NewFromUtf8Literal(isolate, "done", NewStringType::kInternalized);
    bool done = step_obj->Get(ctx, done_str).ToLocalChecked()->BooleanValue(isolate);

    if (done) {
      festate->eof_reached = true;
      if (!festate->is_parameterized && festate->cache_enabled) {
        festate->unparam_cache_complete = true;
      }
      return slot;
    }

    Local<String> val_str = String::NewFromUtf8Literal(isolate, "value", NewStringType::kInternalized);
    Local<Value> row_val = step_obj->Get(ctx, val_str).ToLocalChecked();

    // Populate attributes in ecxt_per_tuple_memory
    ExprContext *econtext = node->ss.ps.ps_ExprContext;
    MemoryContext oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

    plv8_fdw_populate_slot(row_val, slot, festate);
    ExecStoreVirtualTuple(slot);

    // Record into caches if enabled
    if (festate->cache_enabled) {
      if (!festate->is_parameterized && festate->unparam_tuplestore) {
        tuplestore_puttupleslot(festate->unparam_tuplestore, slot);
      } else if (festate->is_parameterized && festate->current_cache_bytes < festate->cache_memory_limit_bytes) {
        MemoryContext scxt = MemoryContextSwitchTo(festate->scan_mcxt);
        if (festate->param_cache.find(festate->current_param_hash) == festate->param_cache.end()) {
          // Check limit and evict oldest if needed
          while (festate->param_cache.size() >= festate->cache_size_limit && !festate->param_lru_order.empty()) {
            uint64 oldest = festate->param_lru_order.back();
            festate->param_lru_order.pop_back();
            auto old_it = festate->param_cache.find(oldest);
            if (old_it != festate->param_cache.end()) {
              festate->current_cache_bytes -= old_it->second.memory_used;
              for (auto mt_old : old_it->second.tuples) {
                pfree(mt_old);
              }
              festate->param_cache.erase(old_it);
            }
          }
          festate->param_lru_order.push_front(festate->current_param_hash);
        }
        MinimalTuple mt = ExecCopySlotMinimalTuple(slot);
        festate->param_cache[festate->current_param_hash].tuples.push_back(mt);
        size_t mt_size = mt->t_len;
        festate->param_cache[festate->current_param_hash].memory_used += mt_size;
        festate->current_cache_bytes += mt_size;
        MemoryContextSwitchTo(scxt);
      }
    }

    MemoryContextSwitchTo(oldcxt);
    festate->stats_rows_fetched++;
    return slot;
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
  return slot;
}

/*
 * FDW Scan Callback: ReScanForeignScan
 */
static void
plv8_fdw_rescan_foreign_scan(ForeignScanState *node)
{
  Plv8FdwScanState *festate = (Plv8FdwScanState *) node->fdw_state;
  if (!festate) return;

  festate->eof_reached = false;

  // Tier 1: Rewind Tuplestore directly
  if (!festate->is_parameterized && festate->unparam_cache_complete && festate->unparam_tuplestore) {
    tuplestore_rescan(festate->unparam_tuplestore);
    festate->stats_cache_hits++;
    return;
  }

  // Tier 2: Check parameter LRU cache
  if (festate->is_parameterized) {
    uint64 hash = plv8_fdw_eval_param_hash(festate, node);
    festate->current_param_hash = hash;

    if (festate->cache_enabled) {
      auto it = festate->param_cache.find(hash);
      if (it != festate->param_cache.end()) {
        festate->replaying_from_cache = true;
        festate->cached_tuple_cursor = 0;
        festate->stats_cache_hits++;
        // Touch LRU entry
        festate->param_lru_order.remove(hash);
        festate->param_lru_order.push_front(hash);
        return;
      }
    }
  }

  // Cache miss
  festate->stats_cache_misses++;
  festate->replaying_from_cache = false;
  festate->cached_tuple_cursor = 0;

  if (!festate->iterator.IsEmpty()) {
    festate->iterator.Reset();
  }

  try {
    plv8_fdw_invoke_execute(festate, node);
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
}

/*
 * FDW Scan Callback: EndForeignScan
 */
static void
plv8_fdw_end_foreign_scan(ForeignScanState *node)
{
  Plv8FdwScanState *festate = (Plv8FdwScanState *) node->fdw_state;
  if (!festate) return;

  try {
    Isolate *isolate = festate->isolate;
    if (isolate && !festate->context.IsEmpty()) {
      Isolate::Scope iso_scope(isolate);
      HandleScope hs(isolate);
      Local<Context> ctx = festate->context.Get(isolate);
      Context::Scope ctx_scope(ctx);

      if (!festate->wrapper_instance.IsEmpty()) {
        Local<Object> wrapper = festate->wrapper_instance.Get(isolate);
        Local<Value> close_val;
        if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "close")).ToLocal(&close_val) && close_val->IsFunction()) {
          Local<Function> close_fn = close_val.As<Function>();
          TryCatch tc(isolate);
          (void) close_fn->Call(ctx, wrapper, 0, nullptr);
        }
      }
    }
  }
  catch (...) {}

  if (festate->unparam_tuplestore) {
    tuplestore_end(festate->unparam_tuplestore);
    festate->unparam_tuplestore = nullptr;
  }

  MemoryContext scan_mcxt = festate->scan_mcxt;
  node->fdw_state = nullptr;

  if (scan_mcxt) {
    MemoryContextDelete(scan_mcxt);
  }
}

/*
 * FDW Modify Callback: AddForeignUpdateTargets
 */
static void
plv8_fdw_add_foreign_update_targets(PlannerInfo *root, Index rtindex, RangeTblEntry *target_rte, Relation target_relation)
{
  std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(RelationGetRelid(target_relation));
  std::string rowid_col = "id";
  auto it = opts.find("rowid_column");
  if (it != opts.end()) {
    rowid_col = it->second;
  } else {
    auto it_key = opts.find("key");
    if (it_key != opts.end()) rowid_col = it_key->second;
  }

  TupleDesc tupdesc = RelationGetDescr(target_relation);
  AttrNumber rowid_attno = InvalidAttrNumber;
  Oid rowid_type = InvalidOid;
  int32 rowid_typmod = -1;
  Oid rowid_coll = InvalidOid;

  for (int i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
    if (!attr->attisdropped && (strcmp(NameStr(attr->attname), rowid_col.c_str()) == 0 ||
                               strcmp(NameStr(attr->attname), "key") == 0)) {
      rowid_attno = attr->attnum;
      rowid_type = attr->atttypid;
      rowid_typmod = attr->atttypmod;
      rowid_coll = attr->attcollation;
      break;
    }
  }

  if (rowid_attno == InvalidAttrNumber && tupdesc->natts > 0) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, 0);
    rowid_attno = attr->attnum;
    rowid_type = attr->atttypid;
    rowid_typmod = attr->atttypmod;
    rowid_coll = attr->attcollation;
  }

  Var *var = makeVar(rtindex, rowid_attno, rowid_type, rowid_typmod, rowid_coll, 0);
  add_row_identity_var(root, var, rtindex, "rowid");
}

/*
 * FDW Modify Callback: PlanForeignModify
 */
static List *
plv8_fdw_plan_foreign_modify(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index)
{
  return NIL;
}

static void
plv8_fdw_modify_cleanup_callback(void *arg)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) arg;
  if (!fmstate) return;

  Isolate *isolate = fmstate->isolate;
  if (isolate) {
    Isolate::Scope iso_scope(isolate);
    fmstate->wrapper_instance.Reset();
    fmstate->context.Reset();
    for (auto &cn : fmstate->colnames) {
      cn.Reset();
    }
    fmstate->colnames.clear();
    for (auto &br : fmstate->batch_rows) {
      br.Reset();
    }
    fmstate->batch_rows.clear();
    fmstate->~Plv8FdwModifyState();
  } else {
    fmstate->~Plv8FdwModifyState();
  }
}

/*
 * FDW Modify Callback: BeginForeignModify
 */
static void
plv8_fdw_begin_foreign_modify(ModifyTableState *mtstate, ResultRelInfo *rinfo, List *fdw_private, int subplan_index, int eflags)
{
  Relation rel = rinfo->ri_RelationDesc;
  Oid foreigntableid = RelationGetRelid(rel);

  try {
    plv8_context *context = GetPlv8Context();
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
    Local<Object> wrapper = plv8_fdw_get_wrapper_instance(context, foreigntableid, opts);

    MemoryContext query_cxt = mtstate->ps.state->es_query_cxt;
    MemoryContext modify_mcxt = AllocSetContextCreate(
        query_cxt,
        "plv8_fdw modify context",
        ALLOCSET_DEFAULT_SIZES);
    MemoryContext oldcxt = MemoryContextSwitchTo(modify_mcxt);

    Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) palloc0(sizeof(Plv8FdwModifyState));
    new (fmstate) Plv8FdwModifyState();
    fmstate->isolate = isolate;
    fmstate->context.Reset(isolate, ctx);
    fmstate->wrapper_instance.Reset(isolate, wrapper);
    fmstate->operation = mtstate->operation;
    fmstate->modify_mcxt = modify_mcxt;

    fmstate->cleanup_cb.func = plv8_fdw_modify_cleanup_callback;
    fmstate->cleanup_cb.arg = fmstate;
    MemoryContextRegisterResetCallback(modify_mcxt, &fmstate->cleanup_cb);

    TupleDesc tupdesc = RelationGetDescr(rel);
    fmstate->tupdesc = tupdesc;
    fmstate->coltypes.resize(tupdesc->natts);
    fmstate->colnames.resize(tupdesc->natts);
    fmstate->colname_strs.resize(tupdesc->natts);

    for (int i = 0; i < tupdesc->natts; i++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (attr->attisdropped) continue;
      memset(&fmstate->coltypes[i], 0, sizeof(plv8_type));
      plv8_fill_type(&fmstate->coltypes[i], attr->atttypid, modify_mcxt);
      char *cn = NameStr(attr->attname);
      fmstate->colname_strs[i] = cn;
      fmstate->colnames[i].Reset(isolate, String::NewFromUtf8(isolate, cn, NewStringType::kNormal).ToLocalChecked());
    }

    std::string rowid_col = "id";
    auto it_r = opts.find("rowid_column");
    if (it_r != opts.end()) rowid_col = it_r->second;
    else {
      auto it_k = opts.find("key");
      if (it_k != opts.end()) rowid_col = it_k->second;
    }
    fmstate->rowid_colname = rowid_col;
    fmstate->rowid_type = TEXTOID;

    for (int i = 0; i < tupdesc->natts; i++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (!attr->attisdropped && (strcmp(NameStr(attr->attname), rowid_col.c_str()) == 0 ||
                                 strcmp(NameStr(attr->attname), "key") == 0)) {
        fmstate->rowid_type = attr->atttypid;
        break;
      }
    }

    if (mtstate->operation == CMD_UPDATE || mtstate->operation == CMD_DELETE) {
      Plan *subplan = outerPlanState(mtstate)->plan;
      fmstate->rowid_attno = ExecFindJunkAttributeInTlist(subplan->targetlist, "rowid");
    }

    fmstate->batch_size = 1;
    auto b_it = opts.find("batch_size");
    if (b_it != opts.end()) {
      fmstate->batch_size = std::max(1, std::atoi(b_it->second.c_str()));
    }

    rinfo->ri_FdwState = (void *) fmstate;
    MemoryContextSwitchTo(oldcxt);
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
}

/*
 * Helper: serializes a tuple slot to a JavaScript object.
 */
static Local<Object>
plv8_fdw_slot_to_js(Isolate *isolate, Local<Context> ctx, TupleTableSlot *slot, TupleDesc tupdesc, const std::vector<plv8_type> &coltypes)
{
  slot_getallattrs(slot);
  Local<Object> obj = Object::New(isolate);

  for (int i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
    if (attr->attisdropped) continue;

    Local<String> col_name = String::NewFromUtf8(isolate, NameStr(attr->attname), NewStringType::kNormal).ToLocalChecked();
    if (slot->tts_isnull[i]) {
      (void) obj->Set(ctx, col_name, v8::Null(isolate));
    } else {
      plv8_type pt = coltypes[i];
      Local<Value> v = ToValue(slot->tts_values[i], false, &pt);
      (void) obj->Set(ctx, col_name, v);
    }
  }
  return obj;
}

/*
 * FDW Modify Callback: ExecForeignInsert
 */
static TupleTableSlot *
plv8_fdw_exec_foreign_insert(EState *estate, ResultRelInfo *rinfo, TupleTableSlot *slot, TupleTableSlot *planSlot)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) rinfo->ri_FdwState;
  if (!fmstate) return slot;

  try {
    Isolate *isolate = fmstate->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = fmstate->context.Get(isolate);
    Context::Scope ctx_scope(ctx);

    Local<Object> wrapper = fmstate->wrapper_instance.Get(isolate);
    Local<Object> row = plv8_fdw_slot_to_js(isolate, ctx, slot, fmstate->tupdesc, fmstate->coltypes);

    Local<Value> ins_val;
    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "insert")).ToLocal(&ins_val) && ins_val->IsFunction()) {
      Local<Function> ins_fn = ins_val.As<Function>();
      Local<Value> arg = row;
      TryCatch tc(isolate);
      (void) ins_fn->Call(ctx, wrapper, 1, &arg);
      if (tc.HasCaught()) throw js_error(tc);
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
  return slot;
}

/*
 * FDW Modify Callback: ExecForeignBatchInsert
 */
static TupleTableSlot **
plv8_fdw_exec_foreign_batch_insert(EState *estate, ResultRelInfo *rinfo, TupleTableSlot **slots, TupleTableSlot **planSlots, int *numSlots)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) rinfo->ri_FdwState;
  if (!fmstate || *numSlots <= 0) return slots;

  try {
    Isolate *isolate = fmstate->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = fmstate->context.Get(isolate);
    Context::Scope ctx_scope(ctx);

    Local<Object> wrapper = fmstate->wrapper_instance.Get(isolate);
    Local<Array> rows_arr = Array::New(isolate, *numSlots);

    for (int i = 0; i < *numSlots; i++) {
      Local<Object> r = plv8_fdw_slot_to_js(isolate, ctx, slots[i], fmstate->tupdesc, fmstate->coltypes);
      (void) rows_arr->Set(ctx, (uint32_t) i, r);
    }

    Local<Value> batch_fn_val;
    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "batchInsert")).ToLocal(&batch_fn_val) && batch_fn_val->IsFunction()) {
      Local<Function> batch_fn = batch_fn_val.As<Function>();
      Local<Value> arg = rows_arr;
      TryCatch tc(isolate);
      (void) batch_fn->Call(ctx, wrapper, 1, &arg);
      if (tc.HasCaught()) throw js_error(tc);
    } else {
      for (int i = 0; i < *numSlots; i++) {
        (void) plv8_fdw_exec_foreign_insert(estate, rinfo, slots[i], planSlots ? planSlots[i] : nullptr);
      }
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
  return slots;
}

/*
 * FDW Modify Callback: GetForeignModifyBatchSize
 */
static int
plv8_fdw_get_foreign_modify_batch_size(ResultRelInfo *rinfo)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) rinfo->ri_FdwState;
  return fmstate ? fmstate->batch_size : 1;
}

/*
 * FDW Modify Callback: ExecForeignUpdate
 */
static TupleTableSlot *
plv8_fdw_exec_foreign_update(EState *estate, ResultRelInfo *rinfo, TupleTableSlot *slot, TupleTableSlot *planSlot)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) rinfo->ri_FdwState;
  if (!fmstate) return slot;

  try {
    Isolate *isolate = fmstate->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = fmstate->context.Get(isolate);
    Context::Scope ctx_scope(ctx);

    bool isnull = false;
    Datum rowid_datum = ExecGetJunkAttribute(planSlot, fmstate->rowid_attno, &isnull);
    if (isnull) {
      ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("plv8_fdw: rowid junk attribute is null")));
    }

    plv8_type pt;
    memset(&pt, 0, sizeof(pt));
    plv8_fill_type(&pt, fmstate->rowid_type);
    Local<Value> rowid_val = ToValue(rowid_datum, false, &pt);

    Local<Object> new_values = plv8_fdw_slot_to_js(isolate, ctx, slot, fmstate->tupdesc, fmstate->coltypes);
    Local<Object> wrapper = fmstate->wrapper_instance.Get(isolate);

    Local<Value> upd_val;
    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "update")).ToLocal(&upd_val) && upd_val->IsFunction()) {
      Local<Function> upd_fn = upd_val.As<Function>();
      Local<Value> args[2] = {rowid_val, new_values};
      TryCatch tc(isolate);
      (void) upd_fn->Call(ctx, wrapper, 2, args);
      if (tc.HasCaught()) throw js_error(tc);
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
  return slot;
}

/*
 * FDW Modify Callback: ExecForeignDelete
 */
static TupleTableSlot *
plv8_fdw_exec_foreign_delete(EState *estate, ResultRelInfo *rinfo, TupleTableSlot *slot, TupleTableSlot *planSlot)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) rinfo->ri_FdwState;
  if (!fmstate) return slot;

  try {
    Isolate *isolate = fmstate->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = fmstate->context.Get(isolate);
    Context::Scope ctx_scope(ctx);

    bool isnull = false;
    Datum rowid_datum = ExecGetJunkAttribute(planSlot, fmstate->rowid_attno, &isnull);
    if (isnull) {
      ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("plv8_fdw: rowid junk attribute is null")));
    }

    plv8_type pt;
    memset(&pt, 0, sizeof(pt));
    plv8_fill_type(&pt, fmstate->rowid_type);
    Local<Value> rowid_val = ToValue(rowid_datum, false, &pt);

    Local<Object> wrapper = fmstate->wrapper_instance.Get(isolate);
    Local<Value> del_val;
    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "delete")).ToLocal(&del_val) && del_val->IsFunction()) {
      Local<Function> del_fn = del_val.As<Function>();
      TryCatch tc(isolate);
      (void) del_fn->Call(ctx, wrapper, 1, &rowid_val);
      if (tc.HasCaught()) throw js_error(tc);
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
  return slot;
}

/*
 * FDW Modify Callback: EndForeignModify
 */
static void
plv8_fdw_end_foreign_modify(EState *estate, ResultRelInfo *rinfo)
{
  Plv8FdwModifyState *fmstate = (Plv8FdwModifyState *) rinfo->ri_FdwState;
  if (!fmstate) return;

  MemoryContext modify_mcxt = fmstate->modify_mcxt;
  rinfo->ri_FdwState = nullptr;

  if (modify_mcxt) {
    MemoryContextDelete(modify_mcxt);
  }
}

/*
 * FDW Updatability Callback: IsForeignRelUpdatable
 */
static int
plv8_fdw_is_foreign_rel_updatable(Relation rel)
{
  return (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);
}

/*
 * FDW Direct Modify Callbacks: PlanDirectModify, BeginDirectModify, IterateDirectModify, EndDirectModify
 */
static ForeignScan *
plv8_fdw_find_modifytable_subplan(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index)
{
  Plan *subplan = outerPlan(plan);
  if (subplan && IsA(subplan, ForeignScan)) {
    return (ForeignScan *) subplan;
  }
  return nullptr;
}

static bool
plv8_fdw_plan_direct_modify(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index)
{
  CmdType operation = plan->operation;
  if (operation != CMD_UPDATE && operation != CMD_DELETE)
    return false;

  ForeignScan *fscan = plv8_fdw_find_modifytable_subplan(root, plan, resultRelation, subplan_index);
  if (!fscan)
    return false;

  if (fscan->scan.plan.qual != NIL)
    return false;

  Oid foreigntableid = root->simple_rte_array[resultRelation]->relid;
  std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
  auto it_dm = opts.find("direct_modify");
  if (it_dm == opts.end() || (it_dm->second != "true" && it_dm->second != "on"))
    return false;

  fscan->operation = operation;
  fscan->resultRelation = resultRelation;
  fscan->fdw_private = lappend(fscan->fdw_private, makeInteger((int) operation));
  return true;
}

static void
plv8_fdw_begin_direct_modify(ForeignScanState *node, int eflags)
{
  ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
  Oid foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);
  CmdType operation = CMD_UNKNOWN;
  if (plan->fdw_private != NIL) {
    operation = (CmdType) ((::Integer *) linitial(plan->fdw_private))->ival;
  }

  try {
    plv8_context *context = GetPlv8Context();
    CurrentContextScope ccs(context);
    Isolate *isolate = context->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = context->localContext();
    Context::Scope ctx_scope(ctx);

    std::unordered_map<std::string, std::string> opts = plv8_fdw_get_options(foreigntableid);
    Local<Object> wrapper = plv8_fdw_get_wrapper_instance(context, foreigntableid, opts);

    MemoryContext query_cxt = node->ss.ps.state->es_query_cxt;
    MemoryContext direct_mcxt = AllocSetContextCreate(
        query_cxt,
        "plv8_fdw direct modify context",
        ALLOCSET_DEFAULT_SIZES);
    MemoryContext oldcxt = MemoryContextSwitchTo(direct_mcxt);

    Plv8FdwDirectModifyState *dmstate = (Plv8FdwDirectModifyState *) palloc0(sizeof(Plv8FdwDirectModifyState));
    new (dmstate) Plv8FdwDirectModifyState();
    dmstate->isolate = isolate;
    dmstate->context.Reset(isolate, ctx);
    dmstate->wrapper_instance.Reset(isolate, wrapper);
    dmstate->operation = operation;
    dmstate->has_executed = false;
    dmstate->affected_rows = 0;
    dmstate->set_processed = (node->resultRelInfo == nullptr || node->resultRelInfo->ri_projectReturning == nullptr);
    dmstate->direct_mcxt = direct_mcxt;

    TupleDesc tupdesc = RelationGetDescr(node->ss.ss_currentRelation);
    ListCell *lc;
    foreach (lc, plan->fdw_exprs) {
      Node *clause = (Node *) lfirst(lc);
      Plv8Qual q;
      if (plv8_fdw_extract_qual(clause, plan->scan.scanrelid, tupdesc, q)) {
        if (q.is_param && q.right_expr) {
          q.expr_state = ExecInitExpr((Expr *) q.right_expr, (PlanState *) node);
        }
        dmstate->quals.push_back(q);
      }
    }

    node->fdw_state = (void *) dmstate;
    MemoryContextSwitchTo(oldcxt);
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }
}

static TupleTableSlot *
plv8_fdw_iterate_direct_modify(ForeignScanState *node)
{
  Plv8FdwDirectModifyState *dmstate = (Plv8FdwDirectModifyState *) node->fdw_state;
  TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
  if (!dmstate || dmstate->has_executed) {
    return ExecClearTuple(slot);
  }

  dmstate->has_executed = true;

  try {
    Isolate *isolate = dmstate->isolate;
    Isolate::Scope iso_scope(isolate);
    HandleScope hs(isolate);
    Local<Context> ctx = dmstate->context.Get(isolate);
    Context::Scope ctx_scope(ctx);

    Local<Object> wrapper = dmstate->wrapper_instance.Get(isolate);
    Local<Value> dm_fn_val;
    if (wrapper->Get(ctx, String::NewFromUtf8Literal(isolate, "directModify")).ToLocal(&dm_fn_val) && dm_fn_val->IsFunction()) {
      Local<Function> dm_fn = dm_fn_val.As<Function>();
      const char *op_str = (dmstate->operation == CMD_UPDATE) ? "UPDATE" : "DELETE";
      Local<String> op_js = String::NewFromUtf8(isolate, op_str, NewStringType::kNormal).ToLocalChecked();

      ExprContext *econtext = node->ss.ps.ps_ExprContext;
      Local<Array> quals_arr = Array::New(isolate, (int) dmstate->quals.size());
      for (size_t i = 0; i < dmstate->quals.size(); i++) {
        (void) quals_arr->Set(ctx, (uint32_t) i, plv8_fdw_qual_to_js(ctx, isolate, dmstate->quals[i], econtext));
      }

      Local<Value> args[3] = {op_js, quals_arr, Object::New(isolate)};
      TryCatch tc(isolate);
      Local<Value> ret_val;
      if (dm_fn->Call(ctx, wrapper, 3, args).ToLocal(&ret_val) && ret_val->IsNumber()) {
        dmstate->affected_rows = ret_val->Int32Value(ctx).FromMaybe(0);
      }
      if (tc.HasCaught()) throw js_error(tc);
    }

    if (dmstate->set_processed) {
      node->ss.ps.state->es_processed += dmstate->affected_rows;
    }
  }
  catch (js_error &e) { e.rethrow(); }
  catch (pg_error &e) { e.rethrow(); }
  catch (const std::exception &e) {
    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("plv8_fdw: %s", e.what())));
  }

  return ExecClearTuple(slot);
}

static void
plv8_fdw_end_direct_modify(ForeignScanState *node)
{
  Plv8FdwDirectModifyState *dmstate = (Plv8FdwDirectModifyState *) node->fdw_state;
  if (!dmstate) return;

  MemoryContext direct_mcxt = dmstate->direct_mcxt;
  node->fdw_state = nullptr;

  if (direct_mcxt) {
    Isolate *isolate = dmstate->isolate;
    if (isolate) {
      Isolate::Scope iso_scope(isolate);
      dmstate->wrapper_instance.Reset();
      dmstate->context.Reset();
      dmstate->~Plv8FdwDirectModifyState();
    } else {
      dmstate->wrapper_instance.Reset();
      dmstate->context.Reset();
      dmstate->~Plv8FdwDirectModifyState();
    }
    MemoryContextDelete(direct_mcxt);
  }
}

/*
 * FDW Diagnostic Callback: ExplainForeignScan
 */
static void
plv8_fdw_explain_foreign_scan(ForeignScanState *node, ExplainState *es)
{
  Plv8FdwScanState *festate = (Plv8FdwScanState *) node->fdw_state;
  if (!festate) return;

  if (festate->stats_cache_hits > 0 || festate->stats_cache_misses > 0) {
    ExplainPropertyInteger("Cache Hits", "hits", festate->stats_cache_hits, es);
    ExplainPropertyInteger("Cache Misses", "misses", festate->stats_cache_misses, es);
  }
}

/*
 * FDW Statistical Sampling Callback: Plv8FdwAcquireSampleRows
 */
static int
Plv8FdwAcquireSampleRows(Relation relation, int elevel, HeapTuple *rows, int targrows, double *totalrows, double *totaldeadrows)
{
  *totalrows = 100.0;
  *totaldeadrows = 0.0;
  return 0;
}

/*
 * FDW Statistical Sampling Callback: AnalyzeForeignTable
 */
static bool
plv8_fdw_analyze_foreign_table(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages)
{
  *totalpages = 10;
  *func = Plv8FdwAcquireSampleRows;
  return true;
}

/*
 * FDW Schema Discovery Callback: ImportForeignSchema
 */
static List *
plv8_fdw_import_foreign_schema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
  return NIL;
}

extern "C" {
PG_FUNCTION_INFO_V1(plv8_fdw_validator);
PG_FUNCTION_INFO_V1(plv8_fdw_handler);
}

/*
 * Option Validator Callback: plv8_fdw_validator
 */
PGDLLEXPORT Datum
plv8_fdw_validator(PG_FUNCTION_ARGS)
{
  ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
  Oid catalog = PG_GETARG_OID(1);
  (void) catalog;

  static const char *valid_opts[] = {
      "wrapper",
      "cache_results",
      "cache_size",
      "cache_memory_limit",
      "rowid_column",
      "batch_size",
      "key",
      "direct_modify",
      nullptr
  };

  List *options = untransformRelOptions((Datum) PointerGetDatum(array));
  ListCell *lc;

  foreach (lc, options) {
    DefElem *def = (DefElem *) lfirst(lc);
    bool found = false;
    for (int i = 0; valid_opts[i] != nullptr; i++) {
      if (strcmp(def->defname, valid_opts[i]) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      ereport(ERROR,
              (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
               errmsg("plv8_fdw: invalid option \"%s\"", def->defname)));
    }
  }

  PG_RETURN_VOID();
}

/*
 * FDW Handler Entry Point: plv8_fdw_handler
 */
PGDLLEXPORT Datum
plv8_fdw_handler(PG_FUNCTION_ARGS)
{
  FdwRoutine *routine = makeNode(FdwRoutine);

  routine->GetForeignRelSize = plv8_fdw_get_foreign_rel_size;
  routine->GetForeignPaths = plv8_fdw_get_foreign_paths;
  routine->GetForeignPlan = plv8_fdw_get_foreign_plan;
  routine->BeginForeignScan = plv8_fdw_begin_foreign_scan;
  routine->IterateForeignScan = plv8_fdw_iterate_foreign_scan;
  routine->ReScanForeignScan = plv8_fdw_rescan_foreign_scan;
  routine->EndForeignScan = plv8_fdw_end_foreign_scan;

  routine->AddForeignUpdateTargets = plv8_fdw_add_foreign_update_targets;
  routine->PlanForeignModify = plv8_fdw_plan_foreign_modify;
  routine->BeginForeignModify = plv8_fdw_begin_foreign_modify;
  routine->ExecForeignInsert = plv8_fdw_exec_foreign_insert;
  routine->ExecForeignBatchInsert = plv8_fdw_exec_foreign_batch_insert;
  routine->GetForeignModifyBatchSize = plv8_fdw_get_foreign_modify_batch_size;
  routine->ExecForeignUpdate = plv8_fdw_exec_foreign_update;
  routine->ExecForeignDelete = plv8_fdw_exec_foreign_delete;
  routine->EndForeignModify = plv8_fdw_end_foreign_modify;
  routine->IsForeignRelUpdatable = plv8_fdw_is_foreign_rel_updatable;

  routine->PlanDirectModify = plv8_fdw_plan_direct_modify;
  routine->BeginDirectModify = plv8_fdw_begin_direct_modify;
  routine->IterateDirectModify = plv8_fdw_iterate_direct_modify;
  routine->EndDirectModify = plv8_fdw_end_direct_modify;

  routine->ExplainForeignScan = plv8_fdw_explain_foreign_scan;
  routine->AnalyzeForeignTable = plv8_fdw_analyze_foreign_table;
  routine->ImportForeignSchema = plv8_fdw_import_foreign_schema;

  PG_RETURN_POINTER(routine);
}
