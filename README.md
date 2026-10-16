# SnowQuery — A C++ Analytical Database Engine

An educational analytical database engine written in C++20, inspired by modern analytical database architectures (e.g. Snowflake).

> **Disclaimer:** This is an independent educational project, not a replica of Snowflake's proprietary implementation. It does not implement production-grade distributed storage or cloud infrastructure.

---

## Features (Complete Implementation: Phases 1–6)

### Phase 1: Ingestion & In-Memory Foundation
- **CSV Reader** — RFC-4180 compliant finite-state-machine parser supporting quoted fields, escaped quotes, embedded commas, and configurable delimiters.
- **Type Inference** — Automatic inference of `INTEGER`, `FLOAT`, `TEXT`, and `NULL`.
- **Row-Oriented Table** — In-memory polymorphic rows (`std::variant<std::monostate, int64_t, double, std::string>`).
- **Column Store** — Row-to-column transposition and columnar memory storage.
- **Query Metrics** — High-resolution microsecond telemetry (rows examined, rows returned, blocks skipped, threads used).

### Phase 2: SQL Query Execution Engine
- **Tokenizer & Parser** — Recursive-descent parser producing a structured Abstract Syntax Tree (`SelectStmt`).
- **SQL Grammar** — `SELECT` with column projections, `*`, and aliases; `FROM`; `WHERE` with precedence climbing (`AND`, `OR`, `NOT`, parentheses); `ORDER BY` (`ASC`/`DESC`); `LIMIT`; `GROUP BY`.
- **Aggregates** — `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `MIN(col)`, `MAX(col)`.
- **Hash Join** — In-memory hash join for multi-table queries with automatic attribute collision resolution.

### Phase 3: Persistent Columnar Storage & Data Skipping
- **Persistent Binary Format (`.scol`)** — Portable binary columnar file format with magic headers (`0x53434F4C`), schema metadata, and block boundaries.
- **Block-Level Metadata** — Min/max values and null counts stored per block (default 1,024 rows).
- **Data Skipping / Block Pruning** — Evaluates query predicates against block min/max bounds and seeks past non-matching blocks in $O(1)$ without reading column data payloads.
- **File Validation** — Rejects truncated, corrupted, or incompatible files safely.

### Phase 4: Query Planning & Optimization
- **Logical Query Planning** — Translates AST into a tree of logical operators (`LogicalScan`, `LogicalFilter`, `LogicalProject`, `LogicalAggregate`, `LogicalSort`, `LogicalLimit`, `LogicalJoin`).
- **Predicate Pushdown** — Pushes filter predicates down past projections and joins directly to storage scan.
- **Projection Pushdown (Column Pruning)** — Determines referenced columns across the query tree and restricts scan reading exclusively to required columns.
- **EXPLAIN Command** — Displays human-readable execution plans showing operators, pushdowns, estimated row counts, and data-skipping status.

### Phase 5: High Performance & Parallel Execution
- **Vectorized Execution (`VectorBatch`)** — Columnar array chunks for high-throughput batch evaluation.
- **Parallel Scans & Filters** — Uses C++20 `std::thread` to partition rows across hardware threads without lock contention.
- **Parallel Aggregation** — Computes thread-local aggregate maps and merges them into final groups.
- **Sequential Fallback** — Automatically runs sequentially for workloads below 500 rows to avoid thread overhead.
- **Benchmarking Suite (`snowquery_bench`)** — Reproducible benchmarks evaluating scans, selective filters, hash joins, persistent storage I/O, block pruning, and parallel scaling.

### Phase 6: Reliability & Documentation
- **Automated CTest Suite** — 7 test suites with 100% pass rate covering all engine subsystems.
- **Comprehensive Documentation** — Architecture, storage format specification, SQL reference, implementation guide, and benchmark results in `docs/`.

---

## Build Instructions (Windows & MSYS2 UCRT64)

### Prerequisites
- Windows 10/11
- [MSYS2](https://www.msys2.org/) with UCRT64 environment (`gcc`, `cmake`, `ninja`)

```powershell
# In PowerShell, add UCRT64 to PATH:
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

# Configure and build:
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build
```

---

## Running SnowQuery

### 1. Interactive Shell
```powershell
.\build\snowquery.exe --csv data/orders.csv
```

#### Interactive Commands
| Command | Description |
|---------|-------------|
| `SELECT ... ;` | Execute a SQL statement ending with `;` |
| `EXPLAIN ... ;` | Display the optimized query plan |
| `.tables` | List all tables currently loaded in the catalog |
| `.schema [table]` | Display column names and types for table(s) |
| `.load <file.csv\|.scol>` | Load a CSV or persistent columnar file |
| `.save <table_name> <file.scol>` | Persist an in-memory table to binary columnar format |
| `.help` | Show available commands |
| `.quit` / `.exit` | Exit the interactive shell |

### 2. Direct Query Execution via CLI
```powershell
# Execute SQL directly and exit:
.\build\snowquery.exe --csv data/orders.csv --sql "SELECT city, COUNT(*), SUM(amount) FROM orders GROUP BY city ORDER BY SUM(amount) DESC;"

# Inspect the query plan via EXPLAIN:
.\build\snowquery.exe --csv data/orders.csv --sql "EXPLAIN SELECT city, SUM(amount) FROM orders WHERE amount > 100 GROUP BY city;"

# Query directly from persistent columnar storage:
.\build\snowquery.exe --storage data/orders.scol --sql "SELECT * FROM orders WHERE amount > 500;"
```

---

## Running Automated Tests

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
ctest --test-dir build --output-on-failure
```

### Test Suites (7 Suites, 100% Passed)
1. `CSVReaderTests` — Parsing quotes, commas, escapes, delimiters, error handling.
2. `TableTests` — Schema construction, variant values, type inference.
3. `SqlParserTests` — Lexer, AST nodes, operator precedence, parentheses, aggregates, joins, syntax errors.
4. `ExecutorTests` — Filtering, comparisons, sorting, limit, group by, aggregates, hash join, runtime errors.
5. `StorageTests` — Persistent binary round-trip, block pruning, corrupted magic rejection, truncated file rejection.
6. `OptimizerTests` — Logical plan generation, predicate pushdown, projection pushdown, EXPLAIN verification.
7. `ParallelTests` — VectorBatch conversion, parallel scan equivalence, parallel aggregate equivalence, sequential fallback.

---

## Running Benchmarks

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build\snowquery_bench.exe
```

### Measured Performance Highlights (100,000 Rows)
- **Parallel Scan Speedup**: **3.57x** on 4 threads (188.24 ms sequential vs 52.69 ms parallel).
- **Block Pruning Speedup**: **21.8x reduction in read time** (10.78 ms vs 235.35 ms) by skipping 92 of 98 data blocks.
- **Hash Join**: 100,000 fact rows joined with 1,000 dimension rows in < 1 ms.

---

## Documentation Links

- [System Architecture](docs/architecture.md)
- [Storage Format Specification (`.scol`)](docs/storage_format.md)
- [SQL Reference Manual](docs/sql_reference.md)
- [Engineering Implementation Guide](docs/implementation_guide.md)
- [Empirical Benchmark Results](docs/benchmark_results.md)
