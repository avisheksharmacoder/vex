import os
import sys
import time
import threading
import numpy as np

# Ensure python directory is in path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "python")))
import vex

def format_rate(rate):
    if rate >= 1e9:
        return f"{rate / 1e9:,.2f} B/s"
    elif rate >= 1e6:
        return f"{rate / 1e6:,.2f} M/s"
    elif rate >= 1e3:
        return f"{rate / 1e3:,.2f} k/s"
    return f"{rate:,.1f}/s"

def print_header(title):
    print("\n" + "=" * 78)
    print(f" {title.upper()}")
    print("=" * 78)

def benchmark_ingestion(n_rows=250_000):
    print_header(f"1. Ingestion Throughput ({n_rows:,} rows)")
    
    db = vex.open(None, workers=4, chunk_rows=4096)
    db.q("table telemetry { id: u64 key, age: i64, score: f64, active: bool, symbol: str }", sync=True)
    
    # Fast path db.put ingestion
    symbols = ["AAPL", "MSFT", "GOOG", "AMZN"]
    t_start = time.perf_counter()
    for i in range(n_rows):
        row = {
            "id": i + 1,
            "age": 20 + (i % 60),
            "score": (i * 1.5) % 1000.0,
            "active": (i % 2 == 0),
            "symbol": symbols[i % 4]
        }
        db.put("telemetry", row)
    db.flush()
    t_ingest = time.perf_counter() - t_start
    
    bytes_per_row = 33
    total_bytes = n_rows * bytes_per_row
    mb_sec = (total_bytes / (1024 * 1024)) / t_ingest
    rows_sec = n_rows / t_ingest
    
    print(f"  Rows Ingested:        {n_rows:,}")
    print(f"  Columns:              5 (id, age, score, active, symbol)")
    print(f"  Time Taken:           {t_ingest:.3f} s")
    print(f"  Ingestion Rate:       {format_rate(rows_sec)} rows/sec ({rows_sec:,.0f} rows/s)")
    print(f"  Effective Bandwidth:  {mb_sec:.2f} MB/sec")
    
    return db

def benchmark_queries(db, n_rows=250_000):
    print_header("2. OLAP Query & SIMD Scan Throughput")
    
    queries = [
        ("Full Table Scan & Projection (2 cols)", "telemetry id, score", 20),
        ("Numeric Filter (score > 500.0)", "telemetry[score > 500.0] id, score", 20),
        ("Compound Filter (score > 500.0 and age > 40)", "telemetry[score > 500.0 and age > 40] id, score, age", 20),
        ("Global Aggregations (count, sum, min, max)", "telemetry { n: count(), total: sum(score), lo: min(score), hi: max(score) }", 50),
        ("Filter + Morsel Hash Group-By", "telemetry[score > 250.0] by symbol { n: count(), avg_score: sum(score) }", 50),
    ]
    
    for label, query_str, iters in queries:
        # Warmup
        _ = db.q(query_str)
        
        t0 = time.perf_counter()
        for _ in range(iters):
            res = db.q(query_str)
        dt = time.perf_counter() - t0
        
        total_rows_scanned = n_rows * iters
        rows_per_sec = total_rows_scanned / dt
        latency_ms = (dt / iters) * 1000.0
        
        print(f"  * {label}:")
        print(f"      Query:        {query_str}")
        print(f"      Latency:      {latency_ms:.3f} ms / query")
        print(f"      Throughput:   {format_rate(rows_per_sec)} rows/sec")
        print(f"      Rows Scanned: {total_rows_scanned:,} rows across {iters} iters")
        print()

def benchmark_zerocopy_numpy(db):
    print_header("3. Zero-Copy Python Buffer Protocol & NumPy Vector Math")
    
    res = db.q("telemetry score")
    col = res["score"]
    
    # 1. Latency to create zero-copy NumPy array via col.np()
    t0 = time.perf_counter()
    iters = 100_000
    for _ in range(iters):
        arr = col.np()
    dt = time.perf_counter() - t0
    latency_us = (dt / iters) * 1_000_000.0
    
    # 2. Verify zero-copy
    arr1 = col.np()
    arr2 = np.asarray(col)
    
    print(f"  Buffer Protocol Export:")
    print(f"    Export Latency:        {latency_us:.2f} µs per call ({iters:,} iters)")
    print(f"    NumPy Array Shape:     {arr1.shape}, Dtype: {arr1.dtype}, Bytes: {arr1.nbytes:,}")
    print(f"    Buffer Zero-Copy Check: arr1.ctypes.data == arr2.ctypes.data ({arr1.ctypes.data == arr2.ctypes.data})")
    print(f"    Zero-copy write lock:  arr1.flags.writeable is {arr1.flags.writeable} (immutable)")
    
    # 3. Vector math on the zero-copy buffer
    n_math_iters = 500
    t0 = time.perf_counter()
    for _ in range(n_math_iters):
        _s = np.sum(arr1)
        _m = np.mean(arr1)
        _std = np.std(arr1)
    dt_math = time.perf_counter() - t0
    
    math_ops = len(arr1) * 3 * n_math_iters
    ops_per_sec = math_ops / dt_math
    print(f"  NumPy In-Memory Vector Compute (Sum, Mean, Std):")
    print(f"    Compute Latency:       {(dt_math / n_math_iters) * 1000.0:.3f} ms / pass")
    print(f"    Vector Math Rate:      {format_rate(ops_per_sec)} ops/sec")

def benchmark_gil_free_concurrency(db, n_rows=250_000):
    print_header("4. Multi-Threaded Read Concurrency (GIL-Free)")
    
    query = "telemetry[score > 500.0] { cnt: count(), s: sum(score) }"
    
    for num_threads in [1, 2, 4, 8]:
        queries_per_thread = 50
        total_queries = num_threads * queries_per_thread
        
        def worker():
            for _ in range(queries_per_thread):
                _ = db.q(query)
                
        t0 = time.perf_counter()
        threads = [threading.Thread(target=worker) for _ in range(num_threads)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        dt = time.perf_counter() - t0
        
        qps = total_queries / dt
        total_rows_scanned = total_queries * n_rows
        scan_rate = total_rows_scanned / dt
        
        print(f"  Threads: {num_threads:>2} | Duration: {dt:.3f}s | QPS: {qps:>8,.1f} queries/s | Scan Rate: {format_rate(scan_rate)} rows/s")

def benchmark_wal_persistence(tmp_path="/tmp/vex_bench_wal"):
    print_header("5. WAL Persistence & Crash Recovery Speed")
    
    wal_file = os.path.join(tmp_path, "bench.wal")
    if os.path.exists(wal_file):
        os.remove(wal_file)
    os.makedirs(tmp_path, exist_ok=True)
    
    n_batches = 100
    batch_size = 100
    n_rows = n_batches * batch_size
    print(f"  Writing {n_rows:,} rows ({n_batches} batches of {batch_size}) to disk-backed WAL...")
    t0 = time.perf_counter()
    with vex.open(wal_file, workers=4, chunk_rows=4096, fsync=False) as db:
        db.q("table events { id: u64 key, val: f64 }", sync=True)
        for b in range(n_batches):
            items = ", ".join(f"{{ id: {b * batch_size + i + 1}, val: {(b * batch_size + i + 1) * 1.5} }}" for i in range(batch_size))
            db.q(f"events += [{items}]", sync=True)
    t_write = time.perf_counter() - t0
    wal_size_mb = os.path.getsize(wal_file) / (1024 * 1024)
    print(f"    WAL Write + Commit:   {t_write:.3f} s ({n_rows / t_write:,.0f} rows/s, {wal_size_mb:.2f} MB)")
    
    # Now reopen database and measure recovery speed
    t0 = time.perf_counter()
    with vex.open(wal_file, workers=4) as db:
        res = db.q("events { total: sum(val), cnt: count() }")
        t_recovery = time.perf_counter() - t0
        cnt = res["cnt"][0]
        assert cnt == n_rows
        
    print(f"    Reopen & Recovery:    {t_recovery:.3f} s ({n_rows / t_recovery:,.0f} rows recovered/s)")
    
    # Clean up
    if os.path.exists(wal_file):
        os.remove(wal_file)

if __name__ == "__main__":
    print("\n" + "#" * 78)
    print("  VEX COLUMNAR DATABASE ENGINE BENCHMARK SUITE")
    print("#" * 78)
    
    db = benchmark_ingestion(n_rows=250_000)
    benchmark_queries(db, n_rows=250_000)
    benchmark_zerocopy_numpy(db)
    benchmark_gil_free_concurrency(db, n_rows=250_000)
    benchmark_wal_persistence()
    db.close()
    
    print("\n" + "#" * 78)
    print("  BENCHMARK COMPLETE")
    print("#" * 78 + "\n")
