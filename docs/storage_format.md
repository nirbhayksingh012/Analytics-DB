# SnowQuery Persistent Columnar Storage Format (`.scol`)

This document specifies the binary columnar storage format used by the SnowQuery database engine.

---

## 1. File Format Overview

SnowQuery uses a native, portable binary columnar file format (`.scol`) designed for high-throughput analytical query processing, block-level min/max data skipping, and column projection pushdown.

### Design Principles
- **No pointer serialization**: All offsets, lengths, and values are self-contained primitive types.
- **Header Magic and Versioning**: Quick validation of file integrity and format compatibility.
- **Block-level Segmentation**: Rows are partitioned into uniform blocks (default 1,024 rows).
- **Embedded Statistics**: Each block contains min/max values and null counts per column, enabling $O(1)$ block pruning.
- **Contiguous Column Vectors**: Values within each block are stored column by column to maximize CPU cache locality.

---

## 2. Binary Layout

```
+-------------------------------------------------------------+
| File Header                                                 |
|   - Magic: 4 bytes (0x53434F4C = "SCOL")                    |
|   - Version: uint16 (1)                                     |
|   - Table Name: uint32 length + UTF-8 string bytes          |
|   - Column Count: uint32                                    |
|   - Schema Definitions (Column Count entries):              |
|       * Name: uint32 length + UTF-8 string bytes            |
|       * Type: uint8 (1=Integer, 2=Float, 3=Text)            |
|   - Total Row Count: uint64                                 |
|   - Total Block Count: uint32                               |
|   - Block Size (target rows per block): uint32              |
+-------------------------------------------------------------+
| Data Blocks (Total Block Count entries)                     |
|   +-------------------------------------------------------+ |
|   | Block Header                                          | |
|   |   - Block Row Count: uint32                           | |
|   |   - Block Data Bytes: uint64                          | |
|   +-------------------------------------------------------+ |
|   | Column Statistics (Column Count entries)              | |
|   |   - Null Count: uint32                                | |
|   |   - Has Min/Max: uint8 (0 or 1)                       | |
|   |   - Min Value: [typed payload based on column type]   | |
|   |   - Max Value: [typed payload based on column type]   | |
|   +-------------------------------------------------------+ |
|   | Column Data Payloads (Column Count entries)           | |
|   |   - Null Bitmask: uint8[Block Row Count] (0=valid, 1) | |
|   |   - Values Array:                                     | |
|   |       * Integer: int64_t[Block Row Count]             | |
|   |       * Float:   double[Block Row Count]              | |
|   |       * Text:    uint32 length + chars per row        | |
|   +-------------------------------------------------------+ |
+-------------------------------------------------------------+
```

---

## 3. Data Type Encodings

| Data Type | ID | Size / Encoding |
|-----------|----|-----------------|
| `Null`    | 0  | Stored as null bitmask flag |
| `Integer` | 1  | 64-bit signed little-endian integer (`int64_t`) |
| `Float`   | 2  | 64-bit IEEE 754 double precision floating-point (`double`) |
| `Text`    | 3  | Variable-length: `uint32_t` byte length followed by UTF-8 characters |

---

## 4. Block Pruning (Data Skipping)

When a query containing a `WHERE` filter scans a `.scol` file, the engine checks block metadata prior to reading the column data payload:

### Comparison Rules
Given a block with minimum value $M_{min}$ and maximum value $M_{max}$:
- `col = X`: Pruned if $X < M_{min}$ or $X > M_{max}$.
- `col < X`: Pruned if $M_{min} \ge X$.
- `col <= X`: Pruned if $M_{min} > X$.
- `col > X`: Pruned if $M_{max} \le X$.
- `col >= X`: Pruned if $M_{max} < X$.
- `col != X`: Pruned if $M_{min} = M_{max} = X$ (all rows in block equal $X$).

### Logical Operator Rules
- `AND(A, B)`: Pruned if either `can_prune(A)` is true **or** `can_prune(B)` is true.
- `OR(A, B)`: Pruned if both `can_prune(A)` **and** `can_prune(B)` are true.

### Fast $O(1)$ Seeking
When a block is pruned, the engine reads the `uint64_t block_bytes` field from the block header and seeks past the block data in $O(1)$ without disk reads or string parsing.

---

## 5. Corruption and Truncation Defense

The reader strictly verifies:
1. `magic == 0x53434F4C`: Throws `"Invalid file format: bad magic number"` if corrupted.
2. `version == 1`: Throws `"Unsupported storage format version"` on unrecognized versions.
3. String sanity bounds: Enforces maximum string length (100 MB) to prevent buffer overflows from corrupted length headers.
4. Premature EOF: All stream reads verify stream health and throw `"Corrupted columnar file: unexpected EOF or truncated data"`.
