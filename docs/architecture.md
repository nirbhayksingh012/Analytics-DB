# SnowQuery Architecture & System Design

This document details the architectural design and component interactions within the SnowQuery analytical database engine.

---

## 1. System Architecture Overview

```
                      +-----------------------------+
                      |       User SQL / CLI        |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |     SQL Tokenizer & AST     |
                      |      (sql_parser.cpp)       |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |   Logical Query Planner     |
                      |       (optimizer.cpp)       |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |   Rule-Based Optimizer      |
                      |   - Predicate Pushdown      |
                      |   - Projection Pushdown     |
                      |   - Column Pruning          |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |      Execution Engine       |
                      |       (executor.cpp)        |
                      +-----------------------------+
                        /                         \
                       v                           v
        +----------------------------+  +----------------------------+
        |    Parallel Execution      |  | Persistent Columnar Engine |
        |   - Multi-Worker Scans     |  |   - Block Min/Max Pruning  |
        |   - Vector Batch Chunks    |  |   - Direct .scol Reader    |
        |   - Map-Reduce Aggregates  |  |       (storage.cpp)        |
        |       (parallel.cpp)       |  +----------------------------+
        +----------------------------+
```

---

## 2. Core Components

### 2.1 Storage Subsystem (`Table`, `ColumnStore`, `ColumnarStorage`)
- **Row-Oriented Table (`Table`)**: In-memory variant representation (`int64_t`, `double`, `std::string`, `std::monostate`), used for interactive manipulation and ingestion.
- **Column Store (`ColumnStore`)**: In-memory transposed representation mapping column definitions to typed vectors.
- **Persistent Binary Storage (`ColumnarStorage`)**: High-performance binary file format (`.scol`) partitioned into contiguous row blocks (default 1,024 rows). Each block stores per-column statistics (min value, max value, null count) and null bitmasks.

### 2.2 Parser & AST (`sql_parser.hpp`, `sql_parser.cpp`)
- **Tokenizer**: Converts raw SQL strings into typed token streams, tracking character offsets.
- **Recursive-Descent Parser**: Builds an Abstract Syntax Tree (`SelectStmt`) with support for expressions, unary/binary operators, parenthesized groupings, aliases, joins, and aggregates.
- **Error Handling**: Throws structured `SqlError` with descriptive position annotations upon syntax or token mismatches.

### 2.3 Query Optimizer & Logical Planner (`optimizer.hpp`, `optimizer.cpp`)
- **Logical Nodes**: `LogicalScan`, `LogicalFilter`, `LogicalProject`, `LogicalAggregate`, `LogicalSort`, `LogicalLimit`, `LogicalJoin`.
- **Optimization Passes**:
  1. **Predicate Pushdown**: Pushes `WHERE` conditions down to the `LogicalScan` node so that non-matching blocks can be skipped at the storage layer.
  2. **Projection Pushdown / Column Pruning**: Recursively determines all referenced columns across the query tree and restricts scan reading to only the necessary columns.
- **EXPLAIN Subsystem**: Prints the formatted logical execution tree with pushdown annotations and estimated cardinality.

### 2.4 Query Executor & Join Engine (`executor.hpp`, `executor.cpp`)
- **Hash Join**: Implements classical two-phase in-memory hash join. Builds an unordered hash map on the smaller relation's join key, then probes the larger relation, resolving schema collisions by prefixing attributes.
- **Type-Aware Comparisons**: Evaluates polymorphic `Value` variants with proper IEEE 754 float rules and null handling.

### 2.5 Parallel & Vectorized Engine (`parallel.hpp`, `parallel.cpp`)
- **VectorBatch**: Columnar vector chunks for batch processing.
- **Parallel Scan & Filter**: Divides row ranges across `std::thread` workers. Each worker operates in thread-local storage to prevent cache line bouncing and lock contention.
- **Parallel Aggregation**: Workers compute localized aggregate maps. Master merges partition states (sums, counts, mins, maxs) into final result rows.
- **Sequential Fallback**: Automatically switches to sequential execution if the input size is smaller than `min_rows_per_thread` (500 rows).

### 2.6 Telemetry & Metrics (`metrics.hpp`, `metrics.cpp`)
- Tracks wall-clock time via RAII `ScopedTimer` in microsecond resolution.
- Measures rows examined, rows returned, blocks examined, blocks skipped, and threads utilized.
