# SnowQuery SQL Reference Manual

SnowQuery implements an analytical subset of SQL inspired by the ANSI SQL standard and modern analytical query engines.

---

## 1. Syntax Overview

```sql
[EXPLAIN]
SELECT [ * | column1 [AS alias1], column2, aggregate(column3) [AS alias2] ... ]
FROM table_name [AS table_alias]
[JOIN secondary_table [AS join_alias] ON left_col = right_col]
[WHERE boolean_expression]
[GROUP BY column1, column2 ...]
[ORDER BY column_or_aggregate [ASC | DESC] ...]
[LIMIT n];
```

---

## 2. Clauses

### 2.1 `SELECT`
- **Explicit columns**: `SELECT order_id, amount, city ...`
- **Projection all (`*`)**: `SELECT * FROM orders;`
- **Aliases**: `SELECT city AS location, amount * 1.0 AS revenue ...`
- **Aggregations**: `SELECT city, COUNT(*), SUM(amount), MIN(amount), MAX(amount) ...`

### 2.2 `FROM`
Specifies the primary relation registered in the `Catalog`.
```sql
FROM orders
```

### 2.3 `JOIN`
Performs an in-memory hash join between two relations on an equality condition.
```sql
SELECT orders.order_id, customers.name, orders.amount
FROM orders
JOIN customers ON orders.customer_id = customers.id;
```
- Supported join types: `INNER JOIN` (or plain `JOIN`).
- Name collision handling: Duplicate column names from the joined table are automatically prefixed with the table or alias name (e.g. `customers_name`).

### 2.4 `WHERE`
Filters rows using boolean predicates. Supports operator precedence and grouping parentheses:
- **Comparison operators**: `=`, `!=`, `<>`, `<`, `>`, `<=`, `>=`
- **Logical operators**: `AND`, `OR`, `NOT`
- **Precedence**: `NOT` > `AND` > `OR` (parentheses override precedence)

Example:
```sql
WHERE (city = 'New York' OR city = 'Chicago') AND amount > 500.0;
```

### 2.5 `GROUP BY`
Groups input rows by one or more columns and applies aggregate states to each group.
```sql
SELECT city, product, COUNT(*), SUM(quantity)
FROM orders
GROUP BY city, product;
```

### 2.6 `ORDER BY`
Orders result rows based on one or more columns or aggregate aliases:
- Direction: `ASC` (default) or `DESC`
- Multiple sort keys: Evaluated sequentially.
```sql
ORDER BY SUM(amount) DESC, city ASC;
```

### 2.7 `LIMIT`
Restricts output to the specified number of rows:
```sql
LIMIT 10;
```

### 2.8 `EXPLAIN`
Displays the optimized execution plan without materializing result rows:
```sql
EXPLAIN SELECT city, SUM(amount) FROM orders WHERE amount > 100 GROUP BY city;
```

---

## 3. Aggregate Functions

| Function | Argument | Description |
|----------|----------|-------------|
| `COUNT(*)` | `*` | Counts all rows in the group, including null values. |
| `COUNT(col)` | Column | Counts non-null entries in the column. |
| `SUM(col)` | Column (Integer / Float) | Computes the arithmetic sum of non-null values. |
| `MIN(col)` | Column (Comparable) | Finds the minimum non-null value. |
| `MAX(col)` | Column (Comparable) | Finds the maximum non-null value. |

---

## 4. Unsupported SQL Features

The following SQL features are intentionally outside the current scope:
- `HAVING` clause (filters can be pre-applied in `WHERE`)
- Subqueries / CTEs (`WITH` clauses)
- Outer joins (`LEFT OUTER`, `RIGHT OUTER`, `FULL OUTER`)
- Window functions (`OVER (PARTITION BY ...)`)
- `DISTINCT` keyword
- Non-equality join conditions (`ON a.id > b.id`)
- DDL statements (`CREATE TABLE`, `ALTER TABLE`, `DROP TABLE`)
