#include "vx_db.h"
#include "vx_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_sep(const char *title) {
    printf("\n==============================================================================\n");
    printf(" %s\n", title);
    printf("==============================================================================\n");
}

int main(void) {
    printf("\n##############################################################################\n");
    printf("  VEX PURE C ENGINE BENCHMARK SUITE (-O3 -march=native)\n");
    printf("##############################################################################\n");

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.worker_threads = 4;
    cfg.chunk_rows = 4096;
    vx_db_t *db = vx_db_open(NULL, &cfg);
    if (!db) {
        fprintf(stderr, "Failed to open db\n");
        return 1;
    }

    /* 1. DDL */
    vx_result_t *r = vx_db_query(db,
        "table telemetry { id: u64 key, age: i64, score: f64, active: bool, symbol: str }",
        NULL, 0, true);
    vx_result_free(r);

    /* 2. Ingestion Benchmark */
    const uint64_t n_rows = 500000;
    const char *cols[5] = { "id", "age", "score", "active", "symbol" };
    const char *syms[4] = { "AAPL", "MSFT", "GOOG", "AMZN" };

    print_sep("1. C INGESTION THROUGHPUT (500,000 ROWS)");
    double t0 = vx_now_sec();
    for (uint64_t i = 0; i < n_rows; i++) {
        const char *s = syms[i % 4];
        vx_val_t vals[5] = {
            { .type = VX_TYPE_U64,  .v.u64 = i + 1 },
            { .type = VX_TYPE_I64,  .v.i64 = (int64_t)(20 + (i % 60)) },
            { .type = VX_TYPE_F64,  .v.f64 = (double)((i * 3ULL) % 1000ULL) },
            { .type = VX_TYPE_BOOL, .v.b   = ((i & 1ULL) == 0) },
            { .type = VX_TYPE_STR,  .v.str = { .ptr = (char *)s, .len = (uint32_t)strlen(s) } }
        };
        vx_db_put_row(db, "telemetry", cols, vals, 5, false, false);
    }
    vx_db_flush(db);
    double dt_ingest = vx_now_sec() - t0;
    double ingest_rate = (double)n_rows / dt_ingest;
    double ingest_mb = ((double)n_rows * 33.0 / (1024.0 * 1024.0)) / dt_ingest;

    printf("  Rows Ingested:        %lu\n", (unsigned long)n_rows);
    printf("  Columns:              5 (id, age, score, active, symbol)\n");
    printf("  Time Taken:           %.4f s\n", dt_ingest);
    printf("  Ingestion Rate:       %.2f M rows/sec (%.0f rows/s)\n", ingest_rate / 1e6, ingest_rate);
    printf("  Effective Bandwidth:  %.2f MB/sec\n", ingest_mb);

    /* 3. C Query Engine Benchmark */
    print_sep("2. C OLAP QUERY ENGINE BENCHMARKS (500,000 ROWS)");

    struct {
        const char *label;
        const char *query;
        int iters;
    } benchmarks[] = {
        { "Full Scan & Projection (2 cols)", "telemetry id, score", 30 },
        { "Numeric Filter (score > 500.0)", "telemetry[score > 500.0] id, score", 30 },
        { "Compound Filter (score > 500.0 and age > 40)", "telemetry[score > 500.0 and age > 40] id, score, age", 30 },
        { "Global Aggregations (count, sum, min, max)", "telemetry { n: count(), total: sum(score), lo: min(score), hi: max(score) }", 50 },
        { "Filter + Morsel Hash Group-By", "telemetry[score > 250.0] by symbol { n: count(), avg_score: sum(score) }", 50 },
        { NULL, NULL, 0 }
    };

    for (int b = 0; benchmarks[b].label != NULL; b++) {
        const char *lbl = benchmarks[b].label;
        const char *q = benchmarks[b].query;
        int iters = benchmarks[b].iters;

        /* Warmup */
        r = vx_db_query(db, q, NULL, 0, false);
        vx_result_free(r);

        t0 = vx_now_sec();
        for (int i = 0; i < iters; i++) {
            r = vx_db_query(db, q, NULL, 0, false);
            if (!r || !r->ok) {
                fprintf(stderr, "Query failed: %s (%s)\n", q, r ? r->err_msg : "null");
                return 1;
            }
            vx_result_free(r);
        }
        double dt = vx_now_sec() - t0;
        double total_rows = (double)n_rows * (double)iters;
        double rows_per_sec = total_rows / dt;
        double lat_ms = (dt / (double)iters) * 1000.0;

        printf("  * %s:\n", lbl);
        printf("      Query:        %s\n", q);
        printf("      Latency:      %.3f ms / query\n", lat_ms);
        printf("      Throughput:   %.2f M rows/sec\n", rows_per_sec / 1e6);
        printf("      Rows Scanned: %lu across %d iters\n\n", (unsigned long)total_rows, iters);
    }

    vx_db_close(db);

    printf("##############################################################################\n");
    printf("  C ENGINE BENCHMARK COMPLETE\n");
    printf("##############################################################################\n\n");
    return 0;
}

