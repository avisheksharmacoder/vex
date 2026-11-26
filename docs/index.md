# Vex Documentation

Welcome to the comprehensive technical documentation for **Vex**, a high-performance C11 in-memory columnar database engine featuring WAL durability, atomic snapshot checkpointing, runtime SIMD vectorization, and a zero-overhead Python native interface.

---

## Documentation Sections

| Document | Description |
| :--- | :--- |
| **[Architecture & Engine Internals](file:///home/avisheks/Documents/dibs/vex/docs/architecture.md)** | Deep architectural dive into memory layout, chunk zone maps, EBR MVCC concurrency, Robin Hood hash indexing, SPSC write mesh, and write-ahead logging. |
| **[Vex Query Language (VQL) Reference](file:///home/avisheks/Documents/dibs/vex/docs/query_language.md)** | Complete guide to the 6-keyword Pythonic query language, left-to-right pipelined syntax, joins, aggregations, safety guards, and point mutations. |
| **[Python Nativeness & Zero-Copy Interop](file:///home/avisheks/Documents/dibs/vex/docs/python_nativeness.md)** | Detailed breakdown of the C-extension architecture, PEP 3118 buffer protocol, GIL-free query execution, NumPy zero-copy views, and lifetime pinning. |
| **[Technical Nuances & Engineering Decisions](file:///home/avisheks/Documents/dibs/vex/docs/technical_nuances.md)** | In-depth analysis of low-level systems engineering decisions: false sharing prevention, Robin Hood PSL invariants, EBR slot recycling, crash safety semantics, and SIMD dispatch. |
| **[Testing & Verification Policy (No Multi-GB Runs)](file:///home/avisheks/Documents/dibs/vex/docs/testing_policy.md)** | Strict engineering mandate prohibiting multi-gigabyte benchmarks/stress tests and defining budget-shrinking validation rules. |
| **[AI Agent Codebase Guide](file:///home/avisheks/Documents/dibs/vex/aidocs/README.md)** | Fast-orientation docs for AI agents: active V1 vs standalone V2 architecture, module map, build/sanitizer commands, and verified current status. |
| **[V2 Engine Architecture & Implementation Tracker](file:///home/avisheks/Documents/dibs/vex/docs/v2-engine/README.md)** | Central tracker, critical path dependency graph, specifications, and verification gates for V2 Engine milestones (M11–M18). |

---

## High-Level System Architecture

The following diagram illustrates the lifecycle of queries, writes, MVCC snapshot reads, and background durability tasks in Vex:

```mermaid
flowchart TD
    subgraph ClientLayer ["Client & Language Layer"]
        PyApp["Python Application"]
        PyBinding["vex_py.c (GIL Released)"]
        Parser["Parser & AST (64KB Arena)"]
    end

    subgraph ReadPath ["Lock-Free Read Engine (MVCC)"]
        EBR["Epoch-Based Reclamation (EBR)"]
        Catalog["Catalog Snapshot (COW)"]
        WorkerPool["Morsel-Driven Worker Pool"]
        SIMDKernels["SIMD / AVX2 Vector Kernels"]
        ZeroCopy["PEP 3118 Buffer / NumPy Zero-Copy"]
    end

    subgraph WritePath ["High-Throughput Write Path"]
        WMesh["Per-Thread SPSC Write Mesh"]
        CommitWorker["Micro-Batch Commit Worker"]
        PKIndex["128-Way Sharded Robin Hood PK Index"]
        DelMaps["Per-Chunk COW Deletion Bitmaps"]
    end

    subgraph DurabilityLayer ["Durability & Storage Layer"]
        WAL["CRC32 Append-Only WAL"]
        Snapshot["Atomic Binary Snapshot (.snap)"]
        OSLock["Exclusive OS flock"]
    end

    PyApp --> PyBinding
    PyBinding --> Parser
    PyBinding -->|Zero-Copy View| ZeroCopy

    PyBinding -->|db.q (Query)| EBR
    EBR --> Catalog
    Catalog --> WorkerPool
    WorkerPool --> SIMDKernels
    ZeroCopy -.->|Pins Root| Catalog

    PyBinding -->|db.put / Mutations| WMesh
    WMesh --> CommitWorker
    CommitWorker --> PKIndex
    CommitWorker --> DelMaps
    CommitWorker --> WAL
    CommitWorker -->|Publish Root| EBR

    CommitWorker -.->|db.checkpoint| Snapshot
    Snapshot -.->|Monotonic Reset| WAL
    OSLock -.->|Protects| WAL
```

---

## Key Performance & Design Tenets

1. **Zero-Copy Where It Matters**: Columnar chunks are stored with 64-byte memory alignment. Analytical query outputs can be passed directly to NumPy (`res["col"].np()`) or Python memoryviews without memory duplication or type marshalling.
2. **Never Block Readers**: By utilizing Epoch-Based Reclamation (EBR) coupled with Copy-On-Write (COW) catalogs and per-chunk deletion bitsets, reader queries run completely lock-free without encountering atomic contention on catalog access.
3. **No Sentinel Restrictions**: The primary key index implements Robin Hood hashing with 1-based Probe Sequence Length (PSL), permitting all 64-bit integer values (including `0` and `UINT64_MAX`) as valid keys.
4. **Resilient Portability**: CPU feature detection (`__builtin_cpu_supports`) detects AVX2 availability at startup and selects optimized vector kernels while falling back cleanly to scalar operations without requiring fatal `-march=native` compilation flags in portable wheels.

