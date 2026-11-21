import os
import sys
import time
import sqlite3
import numpy as np

# Ensure python directory is in path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "python")))
import vex
import duckdb
import polars as pl

def format_rate(rate):
    if rate >= 1e9:
        return f"{rate / 1e9:,.2f} B/s"
    elif rate >= 1e6:
        return f"{rate / 1e6:,.2f} M/s"
    elif rate >= 1e3:
        return f"{rate / 1e3:,.2f} k/s"
    return f"{rate:,.1f}/s"

def print_banner(text):
    print("\n" + "=" * 80)
    print(f"  {text}")
    print("=" * 80)

def generate_data(n_rows):
    print(f"Generating {n_rows:,} rows of benchmark data in RAM...")
    t0 = time.perf_counter()
    
    symbols_cycle = ["AAPL", "MSFT", "GOOG", "AMZN"]
    ids = np.arange(1, n_rows + 1, dtype=np.uint64)
    ages = (20 + (ids % 60)).astype(np.int64)
    scores = ((ids * 1.5) % 1000.0).astype(np.float64)
    symbols = [symbols_cycle[i % 4] for i in range(n_rows)]
    
    dt = time.perf_counter() - t0
    print(f"Data generated in {dt:.3f} s (100% RAM)\n")
    return ids, ages, scores, symbols

def run_benchmarks(n_rows=500_000):
    print_banner(f"IN-MEMORY SHOWDOWN: VEX vs DUCKDB vs POLARS vs SQLITE ({n_rows:,} rows)")
    print("  * Storage Mode: STRICTLY IN-MEMORY (0 bytes written to disk)")
    print("  * Hardware: Intel Core i5 (4 cores, 8 threads, AVX2)")
    
    ids, ages, scores, symbols = generate_data(n_rows)
    
    # -------------------------------------------------------------
    # 1. SETUP / INGESTION INTO IN-MEMORY DATABASES
    # -------------------------------------------------------------
    print_banner("1. INGESTION INTO IN-MEMORY STORAGE")
    
    # Polars in-memory DataFrame
    t0 = time.perf_counter()
    polars_df = pl.DataFrame({
        "id": ids,
        "age": ages,
        "score": scores,
        "symbol": symbols
    })
    polars_ingest_time = time.perf_counter() - t0
    polars_rate = n_rows / polars_ingest_time
    print(f"  [Polars]  Time: {polars_ingest_time:.3f}s | Rate: {format_rate(polars_rate)} rows/s (Arrow DataFrame)")
    
    # DuckDB in-memory table
    t0 = time.perf_counter()
    duck_con = duckdb.connect(":memory:")
    duck_con.sql("CREATE TABLE telemetry AS SELECT * FROM polars_df")
    duck_ingest_time = time.perf_counter() - t0
    duck_rate = n_rows / duck_ingest_time
    print(f"  [DuckDB]  Time: {duck_ingest_time:.3f}s | Rate: {format_rate(duck_rate)} rows/s (columnar table)")

    # VEX in-memory
    t0 = time.perf_counter()
    vex_db = vex.open(None, workers=4, chunk_rows=4096)
    vex_db.q("table telemetry { id: u64 key, age: i64, score: f64, symbol: str }", sync=True)
    for i in range(n_rows):
        vex_db.put("telemetry", {
            "id": int(ids[i]),
            "age": int(ages[i]),
            "score": float(scores[i]),
            "symbol": symbols[i]
        })
    vex_db.flush()
    vex_ingest_time = time.perf_counter() - t0
    vex_rate = n_rows / vex_ingest_time
    print(f"  [VEX]     Time: {vex_ingest_time:.3f}s | Rate: {format_rate(vex_rate)} rows/s (via db.put)")

    # SQLite :memory: baseline
    t0 = time.perf_counter()
    sql_con = sqlite3.connect(":memory:")
    sql_con.execute("CREATE TABLE telemetry (id INTEGER PRIMARY KEY, age INTEGER, score REAL, symbol TEXT)")
    sql_rows = [(int(ids[i]), int(ages[i]), float(scores[i]), symbols[i]) for i in range(n_rows)]
    sql_con.executemany("INSERT INTO telemetry VALUES (?, ?, ?, ?)", sql_rows)
    sql_con.commit()
    sql_ingest_time = time.perf_counter() - t0
    sql_rate = n_rows / sql_ingest_time
    print(f"  [SQLite]  Time: {sql_ingest_time:.3f}s | Rate: {format_rate(sql_rate)} rows/s (executemany)")

    # -------------------------------------------------------------
    # 2. NUMERIC SIMD FILTER SCAN
    # -------------------------------------------------------------
    print_banner("2. FILTER SCAN: score > 500.0 (Project id, score)")
    iters = 25
    
    # VEX
    _ = vex_db.q("telemetry[score > 500.0] id, score")
    t0 = time.perf_counter()
    for _ in range(iters):
        r = vex_db.q("telemetry[score > 500.0] id, score")
    vex_dt = (time.perf_counter() - t0) / iters
    print(f"  [VEX]     {vex_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / vex_dt)} rows/s")
    
    # DuckDB
    _ = duck_con.execute("SELECT id, score FROM telemetry WHERE score > 500.0").fetchall()
    t0 = time.perf_counter()
    for _ in range(iters):
        r = duck_con.execute("SELECT id, score FROM telemetry WHERE score > 500.0").fetchall()
    duck_dt = (time.perf_counter() - t0) / iters
    print(f"  [DuckDB]  {duck_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / duck_dt)} rows/s")
    
    # Polars
    _ = polars_df.filter(pl.col("score") > 500.0).select(["id", "score"])
    t0 = time.perf_counter()
    for _ in range(iters):
        r = polars_df.filter(pl.col("score") > 500.0).select(["id", "score"])
    polars_dt = (time.perf_counter() - t0) / iters
    print(f"  [Polars]  {polars_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / polars_dt)} rows/s")
    
    # SQLite
    _ = sql_con.execute("SELECT id, score FROM telemetry WHERE score > 500.0").fetchall()
    t0 = time.perf_counter()
    for _ in range(5):  # 5 iters since SQLite is slow
        r = sql_con.execute("SELECT id, score FROM telemetry WHERE score > 500.0").fetchall()
    sql_dt = (time.perf_counter() - t0) / 5
    print(f"  [SQLite]  {sql_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / sql_dt)} rows/s")

    # -------------------------------------------------------------
    # 3. COMPOUND FILTER (score > 500.0 AND age > 40)
    # -------------------------------------------------------------
    print_banner("3. COMPOUND FILTER: score > 500.0 AND age > 40")
    
    # VEX
    _ = vex_db.q("telemetry[score > 500.0 and age > 40] id, score, age")
    t0 = time.perf_counter()
    for _ in range(iters):
        r = vex_db.q("telemetry[score > 500.0 and age > 40] id, score, age")
    vex_dt = (time.perf_counter() - t0) / iters
    print(f"  [VEX]     {vex_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / vex_dt)} rows/s")
    
    # DuckDB
    _ = duck_con.execute("SELECT id, score, age FROM telemetry WHERE score > 500.0 AND age > 40").fetchall()
    t0 = time.perf_counter()
    for _ in range(iters):
        r = duck_con.execute("SELECT id, score, age FROM telemetry WHERE score > 500.0 AND age > 40").fetchall()
    duck_dt = (time.perf_counter() - t0) / iters
    print(f"  [DuckDB]  {duck_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / duck_dt)} rows/s")
    
    # Polars
    _ = polars_df.filter((pl.col("score") > 500.0) & (pl.col("age") > 40)).select(["id", "score", "age"])
    t0 = time.perf_counter()
    for _ in range(iters):
        r = polars_df.filter((pl.col("score") > 500.0) & (pl.col("age") > 40)).select(["id", "score", "age"])
    polars_dt = (time.perf_counter() - t0) / iters
    print(f"  [Polars]  {polars_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / polars_dt)} rows/s")
    
    # SQLite
    t0 = time.perf_counter()
    for _ in range(5):
        r = sql_con.execute("SELECT id, score, age FROM telemetry WHERE score > 500.0 AND age > 40").fetchall()
    sql_dt = (time.perf_counter() - t0) / 5
    print(f"  [SQLite]  {sql_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / sql_dt)} rows/s")

    # -------------------------------------------------------------
    # 4. GLOBAL AGGREGATION: count, sum, min, max
    # -------------------------------------------------------------
    print_banner("4. GLOBAL AGGREGATION: count(), sum(score), min(score), max(score)")
    agg_iters = 50
    
    # VEX
    _ = vex_db.q("telemetry { n: count(), total: sum(score), lo: min(score), hi: max(score) }")
    t0 = time.perf_counter()
    for _ in range(agg_iters):
        r = vex_db.q("telemetry { n: count(), total: sum(score), lo: min(score), hi: max(score) }")
    vex_dt = (time.perf_counter() - t0) / agg_iters
    print(f"  [VEX]     {vex_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / vex_dt)} rows/s")
    
    # DuckDB
    _ = duck_con.execute("SELECT count(*), sum(score), min(score), max(score) FROM telemetry").fetchall()
    t0 = time.perf_counter()
    for _ in range(agg_iters):
        r = duck_con.execute("SELECT count(*), sum(score), min(score), max(score) FROM telemetry").fetchall()
    duck_dt = (time.perf_counter() - t0) / agg_iters
    print(f"  [DuckDB]  {duck_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / duck_dt)} rows/s")
    
    # Polars
    _ = polars_df.select([pl.len().alias("n"), pl.col("score").sum().alias("total"), pl.col("score").min().alias("lo"), pl.col("score").max().alias("hi")])
    t0 = time.perf_counter()
    for _ in range(agg_iters):
        r = polars_df.select([pl.len().alias("n"), pl.col("score").sum().alias("total"), pl.col("score").min().alias("lo"), pl.col("score").max().alias("hi")])
    polars_dt = (time.perf_counter() - t0) / agg_iters
    print(f"  [Polars]  {polars_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / polars_dt)} rows/s")
    
    # SQLite
    t0 = time.perf_counter()
    for _ in range(5):
        r = sql_con.execute("SELECT count(*), sum(score), min(score), max(score) FROM telemetry").fetchall()
    sql_dt = (time.perf_counter() - t0) / 5
    print(f"  [SQLite]  {sql_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / sql_dt)} rows/s")

    # -------------------------------------------------------------
    # 5. FILTER + HASH GROUP-BY AGGREGATION
    # -------------------------------------------------------------
    print_banner("5. FILTER + HASH GROUP-BY: filter(score > 250) GROUP BY symbol")
    
    # VEX
    _ = vex_db.q("telemetry[score > 250.0] by symbol { n: count(), s: sum(score) }")
    t0 = time.perf_counter()
    for _ in range(agg_iters):
        r = vex_db.q("telemetry[score > 250.0] by symbol { n: count(), s: sum(score) }")
    vex_dt = (time.perf_counter() - t0) / agg_iters
    print(f"  [VEX]     {vex_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / vex_dt)} rows/s")
    
    # DuckDB
    _ = duck_con.execute("SELECT symbol, count(*), sum(score) FROM telemetry WHERE score > 250.0 GROUP BY symbol").fetchall()
    t0 = time.perf_counter()
    for _ in range(agg_iters):
        r = duck_con.execute("SELECT symbol, count(*), sum(score) FROM telemetry WHERE score > 250.0 GROUP BY symbol").fetchall()
    duck_dt = (time.perf_counter() - t0) / agg_iters
    print(f"  [DuckDB]  {duck_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / duck_dt)} rows/s")
    
    # Polars
    _ = polars_df.filter(pl.col("score") > 250.0).group_by("symbol").agg([pl.len().alias("n"), pl.col("score").sum().alias("s")])
    t0 = time.perf_counter()
    for _ in range(agg_iters):
        r = polars_df.filter(pl.col("score") > 250.0).group_by("symbol").agg([pl.len().alias("n"), pl.col("score").sum().alias("s")])
    polars_dt = (time.perf_counter() - t0) / agg_iters
    print(f"  [Polars]  {polars_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / polars_dt)} rows/s")
    
    # SQLite
    t0 = time.perf_counter()
    for _ in range(5):
        r = sql_con.execute("SELECT symbol, count(*), sum(score) FROM telemetry WHERE score > 250.0 GROUP BY symbol").fetchall()
    sql_dt = (time.perf_counter() - t0) / 5
    print(f"  [SQLite]  {sql_dt*1000:>7.2f} ms | Throughput: {format_rate(n_rows / sql_dt)} rows/s")

    # -------------------------------------------------------------
    # 6. ZERO-COPY EXPORT TO NUMPY
    # -------------------------------------------------------------
    print_banner("6. EXPORT / HAND-OFF TO NUMPY ARRAY (score column)")
    n_np_iters = 10_000
    
    # VEX
    res = vex_db.q("telemetry score")
    col = res["score"]
    t0 = time.perf_counter()
    for _ in range(n_np_iters):
        arr = col.np()
    vex_np_dt = ((time.perf_counter() - t0) / n_np_iters) * 1_000_000.0
    print(f"  [VEX]     col.np():               {vex_np_dt:>7.2f} µs (Direct PEP 3118 zero-copy pointer view)")
    
    # DuckDB fetchnumpy()
    t0 = time.perf_counter()
    duck_prepared = duck_con.execute("SELECT score FROM telemetry")
    for _ in range(1_000):
        arr = duck_con.execute("SELECT score FROM telemetry").fetchnumpy()["score"]
    duck_np_dt = ((time.perf_counter() - t0) / 1_000) * 1_000_000.0
    print(f"  [DuckDB]  .fetchnumpy():          {duck_np_dt:>7.2f} µs")
    
    # Polars to_numpy()
    t0 = time.perf_counter()
    for _ in range(1_000):
        arr = polars_df["score"].to_numpy()
    polars_np_dt = ((time.perf_counter() - t0) / 1_000) * 1_000_000.0
    print(f"  [Polars]  df['score'].to_numpy(): {polars_np_dt:>7.2f} µs")
    
    # Clean up in-memory connections
    vex_db.close()
    duck_con.close()
    sql_con.close()
    
    print("\n" + "=" * 80)
    print("  SHOWDOWN BENCHMARK FINISHED (100% IN-MEMORY, ZERO DISK I/O)")
    print("=" * 80 + "\n")

if __name__ == "__main__":
    run_benchmarks(n_rows=500_000)
