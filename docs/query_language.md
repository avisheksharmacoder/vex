# Vex Query Language (VQL) Reference

The **Vex Query Language (VQL)** is a pipeline-oriented analytical query language designed to eliminate the SQL ceremony while matching the conciseness of Python slicing and functional chaining.

---

## 1. Core Principles

1. **Left-to-Right Pipelining**: Every query begins with a source table and flows left-to-right through projections, filters, joins, groupings, and sorts. There are no inverted `SELECT ... FROM ... WHERE` clauses.
2. **Minimal Vocabulary**: Only 6 primary statement keywords exist:
   * `table`
   * `del`
   * `sort`
   * `by`
   * `index`
   * `unique`
3. **Pythonic Slicing & Indexing**: Row indexing (`[42]`) and limits/offsets (`[:10]`, `[20:30]`) use identical semantics to Python lists.
4. **Compile-Time Safety**: Accidental full-table updates or deletions are rejected by the grammar.

---

## 2. Querying & Analytical Pipelines

### 2.1 Table Scans & Column Selection

To scan an entire table:
```text
users
```

To pick specific columns, provide comma-separated identifiers:
```text
users id, name, email
```

### 2.2 Computed Columns & Aliases

Expressions use standard arithmetic operators (`+`, `-`, `*`, `/`) and assign aliases using the `alias: expression` syntax:
```text
orders id, subtotal: price * qty, grand_total: (price * qty) * 1.18
```

---

## 3. Filtering & Slicing

Filters are specified inside square brackets `[...]` immediately following a table identifier or joined expression.

### 3.1 Predicates
* **Equality & Comparisons**: `==`, `!=`, `<`, `<=`, `>`, `>=`
* **Logical AND**: Represented by a comma inside brackets:
  ```text
  users[age >= 21, country == "IN"]
  ```
* **Logical OR & NOT**:
  ```text
  users[(country == "IN" or country == "US") and not is_banned]
  ```
* **Null Checking**:
  ```text
  users[phone != null, verified == true]
  ```

### 3.2 Slicing (Limits and Offsets)
Slice rows using standard Python interval notation:
```text
users[:10]        # Limit 10 (rows 0 to 9)
users[20:50]      # Offset 20, limit 30 (rows 20 to 49)
```

### 3.3 Fast-Path Primary Key Lookup ($O(1)$)
When an integer is supplied inside brackets for a table with a declared primary key, Vex bypasses the scan engine and executes an immediate $O(1)$ lookup via the sharded Robin Hood hash index:
```text
users[1042] name, email
```

---

## 4. Joins

Vex supports inner joins, left outer joins, and full outer joins using mathematical operator symbols.

| Join Type | Operator | Example | Notes |
| :--- | :---: | :--- | :--- |
| **Inner Join** | `&` | `orders & users` | Auto-joins using declared Foreign Key |
| **Left Outer Join** | `+` | `orders + users` | Unmatched left rows preserved |
| **Full Outer Join** | `++` | `orders ++ users(orders.uid == users.id)` | Unmatched rows preserved on both sides |

### 4.1 Auto-Joins (Foreign Key Resolution)
When tables have a declared foreign key relationship (`user_id: u64 -> users.id`), you do not need to specify the join condition:
```text
orders & users
```
Vex automatically maps `orders.user_id == users.id`. If no foreign key exists, omitting the condition is rejected at parse time to prevent unintentional Cartesian products.

### 4.2 Explicit Join Conditions
When joining arbitrary tables or multiple candidate keys:
```text
orders + users(user_id == id)
payments & orders(order_ref == code)
```

---

## 5. Grouping & Aggregations

Group-by queries use the `by` keyword, followed by grouping columns and an aggregation block in braces `{...}`.

```text
orders by user_id { count(), total_spent: sum(amount), avg_spent: avg(amount) }
```

### 5.1 Supported Aggregates

| Function | Description |
| :--- | :--- |
| `count()` | Number of matching rows in group |
| `sum(col)` | Sum of column values (64-bit int or float) |
| `avg(col)` | Arithmetic mean (returns float) |
| `min(col)` | Minimum value found in group |
| `max(col)` | Maximum value found in group |

### 5.2 Multi-Column Grouping
```text
orders by country, status { n: count(), revenue: sum(amount) }
```

### 5.3 Distinct Queries
Using `by` without an aggregation block yields unique combinations of the specified columns:
```text
users by country, city
```

### 5.4 Post-Aggregation Filtering ("HAVING")
Filter aggregate results by appending bracket predicates after the aggregation block:
```text
orders by user_id { total: sum(amount) } [total > 5000.0]
```

---

## 6. Sorting

Sort results using the `sort` keyword. Prepend a minus sign (`-`) to sort descending:

```text
users sort age, -salary
```

Sorts chain naturally with projections and slicing:
```text
orders by user_id { rev: sum(amount) } sort -rev [:5]
```

---

## 7. Multi-Statement Scripts & Binding

Break complex queries into readable steps by assigning intermediate results to identifiers:

```text
top_spenders = orders by user_id { total: sum(amount) } [total > 1000]
top_spenders & users name, total sort -total
```

---

## 8. Data Definition Language (DDL)

### 8.1 Creating Tables
```text
table users {
    id: u64 key,
    name: str,
    country: str,
    age: i64,
    created_at: u64
}
```

#### Foreign Keys
Declare foreign key constraints using `-> target_table.target_column`:
```text
table orders {
    id: u64 key,
    user_id: u64 -> users.id,
    amount: f64,
    paid: bool
}
```

#### Supported Types
* `i64`: 64-bit signed integer
* `u64`: 64-bit unsigned integer (valid as `key`)
* `f64`: 64-bit IEEE double-precision float
* `bool`: Boolean flag (`true` / `false`)
* `str`: UTF-8 string (Umbra-style inline/arena)

### 8.2 Dropping Tables
```text
del table orders
```

---

## 9. Mutations (DML)

### 9.1 Insertions (`+=`)
Insert single records or record lists:
```text
users += { id: 1, name: "Aarav", country: "IN", age: 28 }
users += [
    { id: 2, name: "Diya", country: "IN", age: 32 },
    { id: 3, name: "John", country: "US", age: 45 }
]
```

### 9.2 Upserts (`|=`)
Insert or update existing primary keys:
```text
users |= { id: 1, name: "Aarav", country: "IN", age: 29 }
```

### 9.3 Point Updates (`[key] =`)
Update columns for a specific primary key:
```text
users[1] = { age: 30 }
```

### 9.4 Point Deletions (`del` or `-=`)
```text
del users[1]
users[1] -=
```

### 9.5 Filtered Deletions
```text
del users[age < 18]
```

---

## 10. Safety Guards

To prevent accidental data loss in production environments, VQL enforces strict syntactic safeguards:

1. **Bare `del table` is a syntax error**:
   ```text
   del users        # COMPILE ERROR: Must provide explicit predicate or slice [:]
   del users[:]     # OK: Explicitly wipes table rows
   ```
2. **Unqualified bulk updates are prohibited**:
   ```text
   users = { age: 0 }     # COMPILE ERROR: Cannot overwrite whole table without slice
   ```
3. **Cartesian auto-joins are rejected**:
   Attempting `table_a & table_b` without an explicit condition `(a.col == b.col)` or a declared foreign key schema triggers an immediate compile-time error.

---

## 11. Query Plan Inspection (`?`)

Prefix any query with `?` to generate an execution plan explanation without evaluating data:

```text
? orders[amount > 100] & users name, amount sort -amount [:10]
```

In Python, access the plan output via:
```python
res = db.q("? orders[amount > 100] & users name, amount sort -amount [:10]")
print(res.explain)
```
Output:
```text
-> Limit (offset=0, limit=10)
  -> Top-K Sort (-amount)
    -> Hash Join (orders.user_id == users.id)
      -> Scan orders [Morsel Chunk Pruned: amount > 100.0]
      -> Scan users (all)
```

