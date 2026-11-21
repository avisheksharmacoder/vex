# Technical Nuances & Systems Engineering Decisions

This document details the low-level systems engineering decisions, edge cases, and hardware-level trade-offs implemented throughout the **Vex** codebase.

---

## 1. Robin Hood Hashing vs. Chained & Linear-Probing Maps

Traditional hash tables face severe performance degradation under high load factors or heavy mutation churn:
* **Chained Hash Tables**: Suffer from cache misses due to pointer chasing across linked list nodes on the heap.
* **Standard Linear Probing**: Suffers from primary clustering, where long contiguous clusters of keys dramatically increase worst-case search times.
* **Tombstone Poisoning**: Marking deleted slots with a `TOMBSTONE` sentinel causes delete-heavy workloads to fill the table with tombstones, requiring frequent rehashing even when the number of active keys remains low.

### 1.1 The Vex Solution: Robin Hood with Backward-Shift Deletion

In `src/vx_rhmap.c`, Vex implements an open-addressing hash map with Robin Hood displacement and backward-shift deletion.

```text
Insertion Sequence (Displacing "Rich" by "Poor"):
Slot:       0     1     2     3     4
Key:       [A]   [B]   [C]   ---   ---
PSL:        1     2     1     0     0

Insert [D] (Ideal home = Slot 0):
Step 1: Check Slot 0 -> Occupant [A] has PSL 1. [D] has PSL 1. Keep probing.
Step 2: Check Slot 1 -> Occupant [B] has PSL 2. [D] has PSL 2. Keep probing.
Step 3: Check Slot 2 -> Occupant [C] has PSL 1. [D] has PSL 3!
        [D] is "poorer" (PSL 3 > PSL 1). Swap [D] and [C]!
Step 4: Probe with displaced [C] (PSL 2) into Slot 3 (empty).
Final:
Slot:       0     1     2     3     4
Key:       [A]   [B]   [D]   [C]   ---
PSL:        1     2     3     2     0
```

### 1.2 Backward-Shift Deletion (Zero Tombstones)
When a key is deleted:
1. The slot is not marked with a tombstone.
2. The algorithm scans subsequent slots. As long as the next slot contains a displaced element (`PSL > 1`), it shifts that element backward into the current slot, decrementing its PSL by 1.
3. This process repeats until an empty slot (`PSL == 0`) or an element sitting in its ideal home bucket (`PSL == 1`) is reached.
4. **Benefit**: Deletions actively compact clusters and restore search locality without requiring rehash sweeps.

### 1.3 1-Based PSL & The Zero-Sentinel Invariant
Conventional hash tables reserve sentinels like `0` or `UINT64_MAX` to indicate empty slots, preventing users from storing those keys.

Vex uses **1-based Probe Sequence Lengths**:
* `psl == 0`: Slot is empty.
* `psl == 1`: Element is in its home hash bucket.
* `psl > 1`: Element is displaced by $psl - 1$ slots.

Because occupancy is determined entirely by `psl != 0`, **all 64-bit integer values (including 0 and UINT64_MAX) are valid keys**.

---

## 2. Eliminating False Sharing in the SPSC Write Mesh

In multi-threaded shared-memory architectures, multi-core CPUs manage memory in 64-byte cachelines.

```text
False Sharing Hazard (Unpadded Struct):
+-------------------------------+-------------------------------+
|         Producer tail         |         Consumer head         |
|            (8 bytes)          |            (8 bytes)          |
+-------------------------------+-------------------------------+
|<----------------------- 64-Byte Cacheline ------------------->|
```

If the producer thread updates `tail` and the consumer thread updates `head`, both cores attempt to acquire exclusive MESI cache ownership of the same 64-byte line on every write. This creates severe inter-core bus traffic, reducing write throughput from >40M ops/sec to <3M ops/sec.

### Vex Implementation:
In `src/vx_wmesh.c`, Vex isolates producer and consumer states using C11 `_Alignas(64)`:
```c
struct vx_wmesh_tx {
    _Alignas(64) atomic_size_t tail; /* Producer writes here */
    _Alignas(64) atomic_size_t head; /* Consumer reads here  */
    vx_wmesh_t  *owner;
    vx_write_op_t *ring;
};
```
Each pointer occupies its own dedicated hardware cacheline, enabling true wait-free concurrency across separate CPU cores.

---

## 3. Epoch-Based Reclamation vs. Hazard Pointers

| Feature | Hazard Pointers | Epoch-Based Reclamation (Vex) |
| :--- | :--- | :--- |
| **Read Overhead** | Atomic RMW / Store-Load fence on *every* pointer dereference | **Zero atomics** during column scanning (pin once at query start) |
| **Cache Bouncing** | High (readers frequently mutate global pointer tables) | None (readers only write to their private TLS slot) |
| **Memory Reclamation** | Immediate per-node | Batch deferred (every $K$ retirements) |
| **Analytical Scan SLA** | Sluggish (~15-25% CPU overhead) | **Native bare-metal speed** |

### Reader Slot Recycling for Ephemeral Threads
A notorious flaw in naive EBR implementations is **thread registration exhaustion**: if an application launches 1,000 short-lived threads, allocating a reader slot for each thread without recycling exhausts the fixed reader array.

Vex implements lock-free slot recycling in `src/vx_ebr.c`:
* Slots use an atomic `in_use` flag.
* When a thread attaches, it attempts an atomic CAS (`atomic_compare_exchange_strong_explicit(&reader->in_use, &expected, 1)`).
* When the thread exits, the POSIX thread-local destructor unregisters the slot, setting `pinned_epoch = INACTIVE` with sequential consistency (`memory_order_seq_cst`) and setting `in_use = 0`.
* The slot becomes instantly available for future threads.

---

## 4. Crash-Proof Snapshots: The 4-Step Protocol

Many database engines write snapshots with:
```c
write(fd, data, len);
close(fd);
rename("snap.tmp", "snap"); // VULNERABLE!
```

**The Vulnerability**: While `rename()` is an atomic filesystem operation, modern operating system file systems (ext4, XFS, APFS) buffer dirty metadata and file blocks in memory. In the event of a sudden power outage or OS kernel panic, the destination file name may point to an inode whose data blocks were never flushed to physical NVMe cells, resulting in 0-byte or corrupted snapshot files.

### Vex 4-Step Atomic Checkpoint Protocol:
In `src/vx_snap.c`:
1. **Write Temporary File**: Stream live catalog records and live column rows into `<wal_path>.snap.tmp`.
2. **Flush File Data**: Call `fdatasync(fd)` to force all modified data blocks to non-volatile storage before modifying filesystem directory entries.
3. **Atomic Rename**: Call `rename(tmp_path, snap_path)`, which atomically updates the directory entry to point to the verified snapshot.
4. **Parent Directory Fsync**: Open the parent directory containing the snapshot and invoke `fsync(dir_fd)`:
   ```c
   int dfd = open(dir, O_RDONLY);
   if (dfd >= 0) {
       fsync(dfd);
       close(dfd);
   }
   ```
This guarantees that both the file blocks and directory inode entries are physically durable on disk before the WAL is truncated.

---

## 5. ThreadSanitizer & ASLR Memory Mapping on Linux

When building and testing with ThreadSanitizer (TSan) on Linux x86_64, engineers often encounter this abort:
```text
FATAL: ThreadSanitizer: unexpected memory mapping 0x5...
```

### Technical Root Cause:
ThreadSanitizer uses a fixed virtual memory mapping layout to translate application addresses to shadow memory addresses via bit shifts:
$$\text{ShadowAddr} = (\text{AppAddr} \land \text{0x00007fffffffffff}) \times 4 + \text{0x7e0000000000}$$

On recent Linux distributions (Ubuntu 22.04+, Fedora, Debian), the kernel enables aggressive, high-entropy Address Space Layout Randomization (ASLR). The kernel dynamic linker may map the binary's PIE code or heap into virtual addresses above `0x550000000000`, which clashes with TSan's expected application boundary.

### The Vex Makefile Fix:
In [Makefile](file:///home/avisheks/Documents/dibs/vex/Makefile), every race test target executes under `setarch`:
```makefile
SETARCH := $(shell command -v setarch >/dev/null 2>&1 && echo "setarch $$(uname -m) -R")

test-m9-race: $(BUILD_DIR) $(OBJ_AVX2_RACE)
	$(CC) $(CFLAGS_RACE) $(SRC_M9_COMMON) $(OBJ_AVX2_RACE) tests/test_m9_release.c -o $(BUILD_DIR)/test_m9_race -lm
	$(SETARCH) ./$(BUILD_DIR)/test_m9_race
```
The `-R` flag disables address space randomization for the test process without requiring global host system reconfiguration or root privileges, ensuring reliable, reproducible TSan verification in local development and CI pipelines.

