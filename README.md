# SnowQuery — A C++ Analytical Database Engine

An educational mini analytical database engine in C++20, inspired by the general
engineering concepts used in modern analytical databases.

> **Disclaimer:** This is an independent educational project, not a replica of
> Snowflake's proprietary implementation. It does not implement production-grade
> distributed storage, cloud infrastructure, or Snowflake internals.

## Features (Phase 1)

- **CSV Loading** — Reads CSV files with support for quoted fields, embedded
  commas, configurable delimiters, headers, and error reporting.
- **Type Inference** — Automatically detects INTEGER, FLOAT, and TEXT columns
  from data.
- **Row-Oriented Table** — In-memory table with variant-typed cells
  (`int64_t`, `double`, `std::string`, and NULL).
- **Column Store** — Transposes row data into columnar representation (foundation
  for later phases).
- **Pretty Printing** — Box-drawn table output with schema information.
- **Performance Metrics** — Tracks rows examined, rows returned, and elapsed
  time for each operation.
- **Interactive Shell** — Command-line interface with `.schema` and `.quit`
  commands.
- **Automated Tests** — 48 assertions across CSV reader and Table/ColumnStore
  test suites.

## Planned Features

- Phase 2: SQL query execution (SELECT, WHERE, GROUP BY, ORDER BY, LIMIT, aggregates)
- Phase 3: Persistent columnar storage with block-level min/max metadata and pruning
- Phase 4: Query optimization (predicate pushdown, projection pushdown, EXPLAIN)
- Phase 5: Parallel scans, vectorized processing, hash joins
- Phase 6: Comprehensive tests, benchmarks, documentation

## Prerequisites

- **Windows** with [MSYS2](https://www.msys2.org/) UCRT64 environment
- **GCC** (tested with 16.1.0)
- **CMake** ≥ 3.20
- **Ninja** build system

### Install Build Tools (MSYS2 UCRT64)

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

## Build

```powershell
# From PowerShell, add UCRT64 to PATH:
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

# Configure and build:
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build
```

## Run

```powershell
.\build\snowquery.exe --csv data/orders.csv
```

### Interactive Commands

| Command   | Description           |
|-----------|-----------------------|
| `.schema` | Show table schema     |
| `.quit`   | Exit the shell        |
| `.exit`   | Exit the shell        |

## Run Tests

```powershell
cd build
ctest --output-on-failure
```

## Project Structure

```
SnowQuery/
  CMakeLists.txt              # Build configuration
  data/orders.csv             # Sample dataset (20 orders)
  include/snowquery/
    csv_reader.hpp             # CSV parsing interface
    table.hpp                  # Row-oriented table with typed schema
    column_store.hpp           # Columnar storage interface
    metrics.hpp                # Query telemetry
  src/
    main.cpp                   # CLI entry point
    csv_reader.cpp             # CSV parser (state-machine)
    table.cpp                  # Table + type inference
    column_store.cpp           # Row-to-column transposition
    metrics.cpp                # Scoped timer + metrics printing
  tests/
    test_csv_reader.cpp        # 9 CSV test cases (27 assertions)
    test_table.cpp             # 6 test cases (21 assertions)
```

## Sample Output

```
Loaded table 'orders' (20 rows, 7 columns).

+----------+----------+---------------+----------+----------+---------+------------+
| order_id | customer | city          | product  | quantity | amount  | order_date |
+----------+----------+---------------+----------+----------+---------+------------+
| 1001     | Alice    | New York      | Laptop   | 1        | 1200.50 | 2024-01-15 |
| 1002     | Bob      | San Francisco | Phone    | 2        | 1599.98 | 2024-01-16 |
...
| 1020     | Jack     | New York      | Monitor  | 1        | 349.99  | 2024-02-03 |
+----------+----------+---------------+----------+----------+---------+------------+
20 of 20 rows shown.
Schema: order_id (INTEGER), customer (TEXT), city (TEXT), product (TEXT), quantity (INTEGER), amount (FLOAT), order_date (TEXT)
```

## License

Educational project — use freely for learning purposes.
