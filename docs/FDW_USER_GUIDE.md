<!-- disableFinding(LINE_OVER_80) -->
<!-- disableFinding(HEADING_FUNCTION) -->

# PL/v8 Foreign Data Wrapper (`plv8_fdw`) Developer & User Guide

## 1. Introduction

**`plv8_fdw`** enables developers to author full-featured PostgreSQL Foreign Data Wrappers in modern JavaScript and TypeScript without writing C code or compiling external binaries. It combines the ease of scripting with V8's TurboFan JIT execution, zero-copy slot streaming, and intelligent rescan caching.

---

## 2. Quickstart Tutorial

### 2.1 Installation & Setup

```sql
-- Step 1: Install the PL/v8 extension
CREATE EXTENSION IF NOT EXISTS plv8;

-- Step 2: Register the Foreign Data Wrapper handler and validator
CREATE FOREIGN DATA WRAPPER plv8_fdw
  HANDLER plv8_fdw_handler
  VALIDATOR plv8_fdw_validator;

-- Step 3: Create a foreign server pointing to a wrapper class
CREATE SERVER in_memory_srv
  FOREIGN DATA WRAPPER plv8_fdw
  OPTIONS (wrapper 'ArrayTableFdw');

-- Step 4: Create a foreign table
CREATE FOREIGN TABLE in_memory_users (
  id int,
  username text,
  dept_id int,
  role text,
  salary numeric
)
SERVER in_memory_srv;
```

### 2.2 Querying & Filter Pushdown

```sql
-- Execute query with pushed-down filter and projected columns
SELECT username, salary
FROM in_memory_users
WHERE role = 'eng' AND salary > 125000;

-- Verify filter pushdown and sort elimination using EXPLAIN:
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, username, salary
FROM in_memory_users
WHERE role = 'eng'
ORDER BY id ASC;
```

Expected output:
```text
Foreign Scan on public.in_memory_users
  Output: id, username, salary
  Foreign Quals: (role = 'eng'::text)
```
*(Notice: No `Sort` plan node is required because `ArrayTableFdw` exposes pre-sorted pathkeys on `id`!)*

---

## 3. The `plv8.ForeignDataWrapper` Base Class API

To author a custom wrapper, extend the built-in base class `plv8.ForeignDataWrapper`:

```typescript
export interface Qual {
  field: string;          // Target column name
  operator: string;       // '=', '<', '>', '<=', '>=', '<>', 'LIKE', 'IN', 'IS'
  value: any;             // Evaluated constant or runtime parameter value
  isArray: boolean;       // True for array / IN operators
  isParam: boolean;       // True if value was evaluated from an executor parameter
}

export interface ColumnInfo {
  name: string;
  typeOid: number;
  typeName: string;
  category: string;
  isArray: boolean;
}

export abstract class ForeignDataWrapper {
  readonly options: Record<string, string>;
  readonly columns: Record<string, ColumnInfo>;

  constructor(options: Record<string, string>, columns: Record<string, ColumnInfo>) {
    this.options = options;
    this.columns = columns;
  }

  /** Column name(s) acting as rowid for UPDATE / DELETE (e.g. 'id' or ['dept_id', 'user_id']) */
  get rowidColumn(): string | string[] | null { return null; }

  /** Batch size for multi-row INSERT (default 1 = single row) */
  get batchSize(): number { return 1; }

  /** Cost estimation: returns [estimatedRows, estimatedRowWidthBytes] */
  getRelSize(quals: Qual[], projectedColumns: string[]): [number, number] {
    return [1000, 50];
  }

  /** Remote / pre-sorted sort orders available */
  getPathKeys(): Array<{ column: string; ascending: boolean }> { return []; }

  /** Pushdown capability predicate */
  canPushdown(qual: Qual): boolean { return true; }

  /** Primary scan generator yielding JavaScript row objects */
  abstract execute(
    quals: Qual[],
    projectedColumns: string[],
    sortKeys?: Array<{ column: string; ascending: boolean }>
  ): Generator<Record<string, any>, void, unknown>;

  /** Optional rescan hook for parameterized nested loop joins */
  rescan?(newQuals: Qual[]): void;

  /** Cleanup hook called on query termination */
  close?(): void;

  /** DML CRUD operations */
  insert?(values: Record<string, any>): Record<string, any> | void;
  update?(rowid: any, newValues: Record<string, any>): Record<string, any> | void;
  delete?(rowid: any): void;
  batchInsert?(rows: Array<Record<string, any>>): void;

  /** Bulk direct modification pushdown */
  directModify?(operation: 'UPDATE' | 'DELETE', quals: Qual[], newValues?: Record<string, any>): number;

  /** Statistical sampling for ANALYZE */
  acquireSample?(targetRows: number): Array<Record<string, any>>;
  getEstimatedPages?(): number;

  /** Transaction coordination hooks */
  commit?(): void;
  rollback?(): void;

  /** EXPLAIN diagnostic hook */
  explain?(quals: Qual[], projectedColumns: string[]): string[] | Record<string, string>;
}
```

---

## 4. Reference Implementations

### 4.1 In-Memory Array Table (`ArrayTableFdw`)
*Features: Sequential scan, projected column pruning, filter pushdown, and pre-sorted pathkeys.*

```javascript
class ArrayTableFdw extends plv8.ForeignDataWrapper {
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
    return ['=', '<', '>', '<=', '>=', 'IN'].includes(qual.operator);
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
        if (q.operator === '=' && val !== q.value) { match = false; break; }
        if (q.operator === '<' && !(val < q.value)) { match = false; break; }
        if (q.operator === '>' && !(val > q.value)) { match = false; break; }
        if (q.operator === '<=' && !(val <= q.value)) { match = false; break; }
        if (q.operator === '>=' && !(val >= q.value)) { match = false; break; }
        if (q.operator === 'IN' && !q.value.includes(val)) { match = false; break; }
      }
      if (!match) continue;

      if (!projectedColumns || projectedColumns.length === 0) {
        yield row;
      } else {
        const res = {};
        for (const c of projectedColumns) res[c] = row[c];
        yield res;
      }
    }
  }
}
```

### 4.2 In-Memory Map/Dict Table (`MapTableFdw`)
*Features: O(1) primary key lookup, batch lookups, full DML, and transactional undo logging.*

```javascript
class MapTableFdw extends plv8.ForeignDataWrapper {
  constructor(options, columns) {
    super(options, columns);
    this._map = new Map([
      ['sess:1', { key: 'sess:1', user_id: 101, status: 'active',  data: '{"theme":"dark"}' }],
      ['sess:2', { key: 'sess:2', user_id: 102, status: 'expired', data: '{"theme":"light"}' }],
      ['sess:3', { key: 'sess:3', user_id: 103, status: 'active',  data: '{"theme":"system"}' }],
      ['sess:4', { key: 'sess:4', user_id: 104, status: 'active',  data: '{"theme":"dark"}' }]
    ]);
    this._undoLog = [];
  }

  get rowidColumn() { return 'key'; }
  get batchSize() { return 100; }

  canPushdown(qual) {
    return ['=', 'IN'].includes(qual.operator);
  }

  *execute(quals, projectedColumns) {
    let targetKey = null;
    let inKeys = null;

    for (const q of quals) {
      if (q.field === 'key') {
        if (q.operator === '=') targetKey = q.value;
        if (q.operator === 'IN') inKeys = new Set(q.value);
      }
    }

    if (targetKey !== null) {
      const row = this._map.get(targetKey);
      if (row) yield row;
      return;
    }

    if (inKeys !== null) {
      for (const k of inKeys) {
        const row = this._map.get(k);
        if (row) yield row;
      }
      return;
    }

    for (const row of this._map.values()) {
      yield row;
    }
  }

  insert(values) {
    if (!values.key) throw new Error("Missing required primary key 'key'");
    const prev = this._map.get(values.key);
    this._undoLog.push({ op: 'insert', key: values.key, prev });
    this._map.set(values.key, { ...values });
    return values;
  }

  update(rowid, newValues) {
    const existing = this._map.get(rowid);
    if (!existing) throw new Error(`Row not found for key: ${rowid}`);
    this._undoLog.push({ op: 'update', key: rowid, prev: { ...existing } });
    const updated = { ...existing, ...newValues, key: rowid };
    this._map.set(rowid, updated);
    return updated;
  }

  delete(rowid) {
    const existing = this._map.get(rowid);
    if (existing) {
      this._undoLog.push({ op: 'delete', key: rowid, prev: { ...existing } });
      this._map.delete(rowid);
    }
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
```

---

## 5. DML Operations & Transaction Coordination

```sql
CREATE SERVER map_srv
  FOREIGN DATA WRAPPER plv8_fdw
  OPTIONS (wrapper 'MapTableFdw');

CREATE FOREIGN TABLE in_memory_sessions (
  key text,
  user_id int,
  status text,
  data jsonb
)
SERVER map_srv
OPTIONS (rowid_column 'key');

-- 1. INSERT new session
INSERT INTO in_memory_sessions (key, user_id, status, data)
VALUES ('sess:5', 105, 'active', '{"theme":"nord"}');

-- 2. UPDATE existing session
UPDATE in_memory_sessions
SET status = 'expired'
WHERE key = 'sess:1';

-- 3. DELETE session
DELETE FROM in_memory_sessions
WHERE key = 'sess:2';

-- 4. Transaction Rollback Verification
BEGIN;
DELETE FROM in_memory_sessions WHERE key = 'sess:5';
ROLLBACK;

-- 'sess:5' remains present after rollback!
SELECT * FROM in_memory_sessions WHERE key = 'sess:5';
```

---

## 6. Table & Server Configuration Options

| Option Name | Target | Default | Description |
| :--- | :--- | :--- | :--- |
| `wrapper` | `SERVER` / `TABLE` | None | Name of the JavaScript class extending `plv8.ForeignDataWrapper`. |
| `cache_results` | `SERVER` / `TABLE` | `'true'` | When enabled, buffers tuples during rescans (Tier 1 Tuplestore for unparameterized scans, Tier 2 LRU memoizer for parameterized joins). |
| `cache_size` | `SERVER` / `TABLE` | `1000` | Maximum number of parameter entries retained in the LRU memoizer. |
| `cache_memory_limit` | `SERVER` / `TABLE` | `'16MB'` | Maximum memory allocated to cached tuples before caching gracefully disables. |
| `rowid_column` | `TABLE` | None | Column name representing the primary key / row identifier for `UPDATE` and `DELETE`. |
| `batch_size` | `SERVER` / `TABLE` | `1` | Multi-row batch size for bulk `INSERT`. |
