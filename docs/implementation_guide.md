# SnowQuery Implementation Guide & Engineering Details

This guide provides an end-to-end technical explanation of how SnowQuery processes data, executes queries, manages columnar storage, and achieves parallel performance.

---

## 1. How CSV Data Enters the Engine
1. **File Reading**: The `csv_reader` module reads the CSV file character-by-character using an explicit finite-state machine (FSM) supporting unquoted fields, quoted fields with embedded commas, escaped quotes (`""`), and CRLF/LF line endings.
2. **Type Inference**: When the table is initialized via `Table::from_csv`, the engine inspects values column-by-column:
   - If every non-empty value parses as an integer (`int64_t`), the column is typed as `DataType::Integer`.
   - If values contain floating-point formats (`3.14`, `1e5`), it resolves to `DataType::Float`.
   - If values contain alphanumeric characters or mixed types, it defaults to `DataType::Text`.
   - Empty values default to `std::monostate` (representing SQL NULL).
3. **Table Creation**: The rows are parsed according to the inferred schema and stored in memory.

---

## 2. How Tables and Columns are Represented
- **`DataType`**: `Null` (0), `Integer` (1), `Float` (2), `Text` (3).
- **`Value`**: Implemented using C++20 `std::variant<std::monostate, int64_t, double, std::string>`.
- **`Row`**: `std::vector<Value>`.
- **`Table`**: Manages a `std::string name_`, `std::vector<ColumnDef> schema_`, and `std::vector<Row> rows_`.
- **`ColumnStore`**: Column-oriented representation holding a vector of `Column` objects, where each column holds a contiguous `std::vector<Value>`.
- **`VectorBatch`**: Fixed-size columnar chunks used for vectorized and batch-oriented operations.

---

## 3. How SQL is Tokenized and Parsed
1. **Lexical Analysis (`tokenize`)**: Scans SQL input and produces a flat array of `Token` structs (tracking type, textual value, and character offset). Keywords such as `SELECT`, `FROM`, `WHERE`, `JOIN`, `GROUP BY`, `ORDER BY`, `LIMIT`, and aggregate functions are identified via a hash lookup.
2. **Recursive-Descent Parser (`parse_sql`)**:
   - `parse_select_list`: Handles `*`, column references with optional aliases (`AS alias`), and aggregate functions (`COUNT(*)`, `SUM(col)`).
   - `parse_where`: Implements precedence climbing for boolean expressions (`OR` has lowest precedence, followed by `AND`, comparisons, and parenthesized sub-expressions).
   - `parse_joins`: Identifies `JOIN table ON left.col = right.col`.
   - `parse_group_by`, `parse_order_by`, `parse_limit`.
3. **AST Structure**: Produces a `SelectStmt` containing expression pointers (`ExprPtr`).

---

## 4. How a Query Becomes a Logical Plan
1. **Logical Plan Construction**: `QueryOptimizer::build_logical_plan` transforms the `SelectStmt` into a tree of logical operators:
   `LogicalLimit` -> `LogicalSort` -> `LogicalProject` -> `LogicalAggregate` -> `LogicalFilter` -> `LogicalJoin` -> `LogicalScan`.
2. **Optimization Passes**:
   - **Predicate Pushdown**: `QueryOptimizer::apply_predicate_pushdown` traverses the plan and pushes `WHERE` predicates down past projections and joins directly to the `LogicalScan` node (`pushed_predicates`). Redundant filter operators are pruned.
   - **Projection Pushdown (Column Pruning)**: `QueryOptimizer::optimize` gathers all column references across the entire plan and populates `LogicalScan::projected_columns`. Only columns needed for projection, filtering, grouping, or sorting are loaded from disk or scanned from memory.

---

## 5. How the Executor Evaluates the Plan
1. **Table Resolution**: The `from_table` is looked up in the `Catalog`.
2. **Join Processing**: If joins exist, the hash join executor runs (see Section 7).
3. **Filtering**: If rows exceed the parallel threshold (500 rows), the parallel scan engine partitions the workload across threads. Otherwise, it runs sequentially.
4. **Aggregation**: If the query contains `GROUP BY` or aggregates, rows are grouped by compound group keys (`\x01` delimiter). Worker partitions run locally and merge.
5. **Sorting**: If `ORDER BY` is specified, `std::sort` is invoked on the result rows using type-aware multi-key comparisons.
6. **Limit**: If `LIMIT` is specified, `result_rows.resize(limit_val)`.
7. **Result Assembly**: The final `Table` and `QueryMetrics` are packaged into an `ExecResult`.

---

## 6. How Columnar Storage and Metadata-Based Pruning Work
1. **Format Structure (`.scol`)**: Binary columnar storage with a magic header (`0x53434F4C`), schema metadata, and block partitions (default 1,024 rows).
2. **Block Metadata**: Each block records null counts, `has_min_max`, `min_val`, and `max_val` for every column.
3. **Pruning Algorithm (`can_prune_block`)**:
   - Compares query filter bounds against block $[min, max]$.
   - If a block contains values $[100, 200]$ and the query filters for `val > 500`, the block is guaranteed to have zero matching rows.
   - The reader reads the `uint64_t block_bytes` length and seeks forward without reading column payloads from disk.

---

## 7. How Multiple Tables are Loaded and Joined
1. **Catalog Registration**: Multiple CSVs or `.scol` files are loaded into `Catalog` using `--csv <path>` or `.load <path>`.
2. **Hash Join Algorithm**:
   - Left side: probe relation (e.g. `orders`).
   - Right side: build relation (e.g. `customers`).
   - The engine builds an `std::unordered_multimap<std::string, size_t>` on the right table's join key.
   - Probes the hash table for every left row, concatenates matching rows, and resolves name collisions by prepending table aliases (`customers_name`).

---

## 8. How Parallel Workers Operate
1. **Thread Partitioning**: `ParallelEngine::resolve_thread_count` detects `std::thread::hardware_concurrency()` (e.g., 12 threads) and partitions the row count into contiguous chunks $[start_i, end_i)$.
2. **Worker Lifecycle**: Workers are launched via `std::thread` / `std::jthread`. Each worker evaluates filters or computes aggregate states into thread-local buffers.
3. **Lock-Free Concurrency**: No shared mutable locks are held during scanning.
4. **Exception Propagation**: Any worker exception is caught as `std::exception_ptr` and rethrown on the main thread after worker termination.

---

## 9. How Metrics and Benchmarks are Collected
- **Telemetry**: `QueryMetrics` tracks microsecond execution time, rows examined, rows returned, blocks examined, blocks skipped, and threads used.
- **Timing**: RAII `ScopedTimer` captures high-resolution timestamps (`std::chrono::high_resolution_clock`).
- **Benchmark Executable**: `snowquery_bench` synthesizes reproducible datasets (10k, 100k rows) and executes benchmarks across all operations.

---

## 10. Implemented vs Unfinished Features

| Feature Area | Status | Notes |
|--------------|--------|-------|
| CSV Ingestion & Type Inference | **Implemented & Tested** | Full RFC-4180 quote support, auto-typing |
| In-Memory Row & Column Store | **Implemented & Tested** | In-memory representations |
| SQL Tokenizer & AST | **Implemented & Tested** | SELECT, WHERE, GROUP BY, ORDER BY, LIMIT, JOIN |
| Operator Precedence & Parens | **Implemented & Tested** | Full boolean operator climbing |
| Aggregate Functions | **Implemented & Tested** | COUNT(*), COUNT(col), SUM, MIN, MAX |
| In-Memory Hash Join | **Implemented & Tested** | Inner equality join with column collision resolution |
| Binary Columnar Storage (`.scol`) | **Implemented & Tested** | Binary header, block metadata, corruption detection |
| Block-Level Data Skipping | **Implemented & Tested** | Pruned 92/98 blocks in benchmark (21.8x speedup) |
| Logical Planner & Optimizer | **Implemented & Tested** | Predicate pushdown, projection pushdown, EXPLAIN |
| Parallel Scans & Aggregations | **Implemented & Tested** | 3.57x speedup on 4 threads; sequential fallback |
| CLI Interactive Shell | **Implemented & Tested** | SQL, .tables, .schema, .load, .save, .help, .quit |
| Outer Joins / Subqueries / HAVING | *Not Implemented* | Beyond scope of educational engine |

---

## 11. Windows & MSYS2 UCRT64 Build Instructions

```powershell
# 1. Set PATH to include MSYS2 UCRT64 toolchain
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

# 2. Configure and build with Ninja
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build

# 3. Run all test suites
ctest --test-dir build --output-on-failure

# 4. Run benchmarks
.\build\snowquery_bench.exe
```
