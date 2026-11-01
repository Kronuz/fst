# Builder registry and traversal: measurements

Measured 2026-11-01 on the `fst.hh` of `fst:3b845a6`, with the benches of `fst:e4fa6a6` and `fst:dbb8f4d` and the mutant tool of `fst:ff95225`. The per-version tables of the changes that produced the current builder (the reference, the 16-byte-slot registry, now) are in [CHANGELOG.md](../../CHANGELOG.md) under 2026-10-31.

## Change

`fst::builder` keeps the path of the previous key as pending nodes. A node that freezes is hashed and looked up in the registry. An equal node already in the output is reused, otherwise the node is written and registered. The registry is a flat open-addressing table:

- Linear probing, at most half full, 1,024 slots to start (8 KiB), doubling when half full.
- A slot is 8 bytes: the low 32 bits of the node's 64-bit hash and the node's 32-bit address. The table index is the stored tag masked, so a doubling moves slots from the stored bits alone and decodes no node.
- Past 4 GiB of output the slots are 16 bytes (a 32-bit tag and a 64-bit address). The switch is tested every 1,024 registered nodes, which cannot add 4 GiB.
- A hit is verified by `same_node()`, which compares the node's final flag and output and every transition's label, output and target with the bytes already written (the output has no trailer yet, so it reads exact bytes). The output bytes do not depend on the hash or the slot width.
- Pending nodes are cleared in place, so freezing a node allocates nothing.

`range` walks with one small frame per level (node pointers and a transition cursor), decodes a transition only when it takes it, and writes the key into a preallocated buffer. `for_each` runs the same walk, `walk<bounded>`, instantiated without the bound tests.

The registry costs 16 to 32 bytes per distinct node while the output is under 4 GiB, because the table doubles; while a table doubles, the old and the new one are both alive.

## Method

The reference is `test/fst_old.hh`, the builder and reader of `fst:12657aa`: one heap `std::string` signature per written node in an `unordered_map`, and a full decoded node per transition in `range`. It produces the same bytes.

Harnesses, all built with CMake `Release` (`-O3 -DNDEBUG`):

- `fst_cmp_bench`: the reference and `fst.hh` in one binary, each row timed in interleaved trials of reference, reference again (A/A) and `fst.hh`. A row prints the best time of each and the median and range of the two ratios over 11 trials (5 for the builds of 20,000 keys and more). A ratio above 1 means `fst.hh` is faster.
- `fst_memory`: the heap each builder allocates at its peak, counted through `operator new`, over 15 key counts from 1,000 to 2 million. The numbers are exact and do not depend on the allocator.
- `fst_build_rss`: the peak resident set of a process that generates and sorts N random 12-letter keys and builds one FST, one process per measurement, so a peak never carries over. The sorted keys are in the number.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
build/fst_cmp_bench /usr/share/dict/words
build/fst_memory
build/fst_build_rss
```

Machines and compilers:

- Apple M4 Pro, macOS, Apple clang 17.0.0 (libc++).
- Intel Core i9-9980HK (8 cores, 16 threads), macOS 15: Apple clang 17.0.0 (libc++), Homebrew LLVM 23.1.2, and Homebrew GCC 16.2.0 (libstdc++).

Timings ran under a hold that serializes measurements on a machine. The per-version tables in the CHANGELOG were taken with one binary per version, best of 3 alternating runs.

## Results

### Peak heap by key count

`fst_memory`, in MiB, for the builder and the reference over the same random 12-letter keys. With libc++ the numbers are the same on the M4 and on the i9 (to a byte); the GCC 16 columns are the i9.

| keys | libc++ reference | libc++ builder | ratio | libstdc++ reference | libstdc++ builder | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 0.5 | 0.2 | 0.41x | 0.5 | 0.2 | 0.42x |
| 5,000 | 2.2 | 1.7 | 0.76x | 2.2 | 1.7 | 0.79x |
| 20,000 | 8.1 | 3.4 | 0.42x | 8.4 | 3.5 | 0.41x |
| 60,000 | 19.8 | 13.5 | 0.68x | 19.9 | 13.9 | 0.70x |
| 100,000 | 33.3 | 13.5 | 0.40x | 33.5 | 13.9 | 0.41x |
| 150,000 | 44.0 | 27.0 | 0.61x | 53.7 | 27.8 | 0.52x |
| 200,000 | 63.4 | 27.0 | 0.43x | 65.5 | 27.8 | 0.42x |
| 250,000 | 73.2 | 54.0 | 0.74x | 73.3 | 55.5 | 0.76x |
| 300,000 | 82.6 | 54.0 | 0.65x | 82.8 | 55.5 | 0.67x |
| 400,000 | 126.7 | 54.0 | 0.43x | 125.7 | 55.5 | 0.44x |
| 600,000 | 152.9 | 108.0 | 0.71x | 154.0 | 111.0 | 0.72x |
| 800,000 | 196.2 | 108.0 | 0.55x | 221.9 | 111.0 | 0.50x |
| 1,000,000 | 254.1 | 108.0 | 0.43x | 254.0 | 111.0 | 0.44x |
| 1,500,000 | 363.4 | 216.0 | 0.59x | 324.8 | 222.0 | 0.68x |
| 1,999,000 | 512.7 | 216.0 | 0.42x | 482.0 | 222.0 | 0.46x |

The builder needs 0.40x to 0.76x of the reference's heap with libc++ and 0.41x to 0.79x with libstdc++. Most of the builder's heap is the registry table, which doubles as the node count grows, so the builder's heap steps (at 5,000, 20,000, 60,000, 150,000, 250,000, 600,000 and 1,500,000 keys) while the reference's grows steadily; the ratio is highest at the sizes just past a step and lowest before the next one.

### Peak resident set

`fst_build_rss`, in MiB, one process per cell, the sorted keys included (24 bytes per key with libc++). The FST and the keys columns are the M4's.

| keys | FST | sorted keys | M4 reference | M4 builder | ratio | i9 reference | i9 builder | ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100,000 | 2.0 | 3.7 | 46 | 23 | 0.50x | 45 | 23 | 0.51x |
| 300,000 | 5.8 | 8.3 | 110 | 76 | 0.68x | 110 | 82 | 0.75x |
| 1,000,000 | 17.4 | 24.3 | 344 | 156 | 0.45x | 344 | 164 | 0.48x |
| 2,000,000 | 31.7 | 47.2 | 646 | 306 | 0.47x | 645 | 312 | 0.48x |
| 3,000,000 | 45.3 | 70.1 | 810 | 585 | 0.72x | 809 | 591 | 0.73x |

The i9 column is Apple clang 17. A process that builds with `fst.hh` peaks at 0.45x to 0.72x of the reference's resident set on the M4 and 0.48x to 0.75x on the i9. The ratio is highest at 300,000 and 3,000,000 keys, where the registry table has just doubled, as in the heap table above.

### Time

`fst_cmp_bench`: each cell is the reference's time divided by `fst.hh`'s (above 1 is faster) and, in parentheses, `fst.hh`'s time in ns per key (ns per call for the `range` and `intersect` rows). The first two rows are the word-list rows (`/usr/share/dict/words`, 235,976 keys); the others are the edge rows on generated keys.

| row | M4 Apple clang 17 | i9 Apple clang 17 | i9 LLVM 23 | i9 GCC 16 |
|---|---:|---:|---:|---:|
| build, word list | 2.53x (98.9) | 3.10x (206) | 3.34x (188) | 3.29x (196) |
| `for_each`, word list | 1.66x (35.2) | 1.29x (61.7) | 1.16x (50.6) | 1.45x (57.7) |
| build 1 random key | 1.16x (288) | 1.49x (706) | 1.50x (699) | 1.32x (788) |
| build 4 random keys | 1.87x (179) | 2.26x (471) | 2.21x (478) | 2.36x (475) |
| build 16 random keys | 3.29x (122) | 4.28x (298) | 4.12x (302) | 4.51x (297) |
| build 64 random keys | 3.96x (84.8) | 5.34x (190) | 5.07x (198) | 5.45x (185) |
| build 256 random keys | 4.14x (78.6) | 5.14x (188) | 4.98x (197) | 5.27x (188) |
| build 1,024 random keys | 3.84x (80.0) | 3.85x (238) | 3.92x (227) | 4.23x (229) |
| build 4,096 random keys | 3.19x (100) | 3.48x (261) | 3.53x (248) | 3.47x (268) |
| build 300k random 12-letter keys | 5.27x (378) | 4.57x (720) | 4.63x (716) | 4.63x (687) |
| `for_each` 200k random keys | 1.71x (40.8) | 1.30x (74.5) | 1.54x (59.5) | 1.75x (63.6) |
| `range` from a 3-letter prefix, first key | 1.64x (102) | 1.54x (282) | 1.57x (284) | 2.02x (251) |
| `range`, first 10 keys | 1.59x (531) | 1.28x (1,169) | 1.40x (1,103) | 2.02x (917) |
| `range`, first 100 keys | 1.54x (4,763) | 1.08x (9,539) | 1.21x (9,008) | 1.75x (7,509) |
| `intersect`, 3-letter prefix, all matches | 0.95x (813) | 1.03x (1,966) | 1.00x (1,853) | 0.93x (2,770) |
| `get`, 1,024 keys | 0.99x (65.6) | 1.01x (113) | 1.01x (112) | 1.00x (118) |
| build 300k decimal keys | 1.94x (35.3) | 2.48x (72.4) | 2.59x (69.1) | 2.29x (74.3) |
| `for_each` 300k decimal keys | 1.96x (4.7) | 1.31x (11.7) | 2.08x (7.8) | 2.40x (8.2) |
| build 20k keys of 200 bytes | 10.01x (8,721) | 5.76x (20,193) | 5.92x (20,866) | 5.48x (19,879) |
| `for_each` 20k keys of 200 bytes | 1.77x (1,127) | 1.43x (1,825) | 2.13x (1,527) | 2.39x (1,516) |

The A/A medians (the reference against itself) are 0.98 to 1.01 on the M4 and 0.97 to 1.03 on the i9 for every row; the widest single A/A range is 0.94 to 1.05 on the M4 and 0.87 to 1.09 on the i9. The M4 ran alone. The i9 carried a load average of about 8.5 from other work on the machine throughout, so its times are upper bounds (the word-list build takes 9% to 17% longer than in the CHANGELOG's runs of 2026-10-31) and the ratios are the figures to read.

- Builds are 1.2x (one key) to 10x faster. From 16 keys up they are 3.2x to 5.5x faster on random keys on every compiler and machine, 1.9x to 2.6x on decimal keys (which the reference already built at 69 ns per key on the M4), and 5.5x to 10x on 200-byte keys.
- `for_each` is 1.2x to 2.4x faster. The least is Apple clang 17 on the i9 (1.29x to 1.43x) and LLVM 23 on the word list (1.16x).
- Short `range` calls (1 and 10 keys) are 1.3x to 2.0x faster; at 100 keys the gain falls to 1.08x on the i9 with Apple clang and rises to 1.75x with GCC.
- `get` is unchanged, 0.99x to 1.01x. `intersect` is the same code as the reference's and measures 0.93x to 1.03x; it still decodes a whole node for each transition.

## Variants not taken

- **A 3/4 load limit.** It needs less memory (0.35x to 0.5x of the reference) and builds 1.6x faster at 300,000 keys on a Linux x86-64 VM, and is 8% slower at 300,000 keys on the M4. The registry stays at 1/2, with the probe lengths it had.
- **A 24-bit tag with a 40-bit address in the 8-byte slot.** A table grows from its stored bits alone only while the tag has at least as many bits as the index, so past 16 million slots a 24-bit tag would decode and rehash every node from the output at each doubling. A 32-bit tag and a 32-bit address keep a doubling to moving slots.
- **The node writer as a function.** The first 8-byte registry built 32 to 1,024 keys 3% to 8% slower than the 16-byte one. Variants of one function, timed in one interleaved run on several machines and compilers, found three causes: the node writer extracted into a function (5% to 10%, gone with `always_inline`), a per-node "narrow or wide" check (3%, now a counter the table already tests), and two slow-path branches (`[[unlikely]]`). Each showed on some compilers only (GCC 15, Apple clang 17 and 21, LLVM 23), and a function nobody touched moved 5% with the code layout.
- **`for_each` through `range` with empty bounds.** GCC removes the dead bound tests; LLVM 23 on the i9 does not, and ran 0.68x to 0.85x of the reference on four key sets (Apple clang 17: 1.1x; M4: 1.5x). A template parameter on "bounded" brought LLVM to 1.06x to 1.25x of the reference, and Apple clang 17 and the M4 gained 1% to 13% over the version before.
- **A vector of frames resized in place.** It lost 25% on short `range` calls: 64 zeroed frames per call. The frames are reserved and pushed.

## Proof

The builder's bytes must not depend on the registry: any hash function and either slot width give the reference's bytes.

- `fst_diff` compares the builder's bytes and the `range` and `for_each` sequences with the reference over random key sets. `fst_diff_wide` repeats it with `FST_TEST_NARROW_LIMIT=70000`, so the registry switches to 16-byte slots at 70 KB, during the larger sets. `fst_cmp_bench` and `fst_build_rss` check the bytes again on the word list and on the random keys of the memory table.
- `fst_collide` gives every node the same hash (`FST_TEST_HASH_MASK=0`), so `same_node()` decides every registry hit, and compares the bytes with the reference over every subset of the keys of length up to 3 over {a, b} and up to 2 over {a, b, c} under four value shapes, plus random sets with wide nodes and multi-byte outputs.
- `fst_memory` fails when the builder's peak heap exceeds the reference's at any of its 15 key counts. Built against `fst.hh` of `fst:7376bd9`, the registry of 16-byte slots, it fails at 8 of the 15 (M4, libc++; 1.04x to 1.44x of the reference, 1.44x at 5,000 keys). With the current header it passes, at 0.40x to 0.76x.

`python3 tools/mutants.py all --also-repo` (M4, Apple clang 17) edits `fst.hh` in 21 ways and runs the tests built against each:

| mutants | killed by | let through |
|---|---|---|
| 11 of `same_node()`: each comparison removed, and "every hit is equal" | `fst_collide`: 11 | `fst_diff` lets 10 through, `fst_test` lets 9 |
| 6 of the registry: the widening drops entries, the wide table never registers or never grows, the narrow table never grows, the switch is never taken or never checked | `fst_diff_wide`: 6 | |
| 4 of the walk: `for_each` emits no key, ignores the stop, drops a transition's output, or never advances | `fst_diff`: 4, `fst_test`: 4 | |

`fst_diff` and `fst_test` do not reach the rejecting branches of `same_node()` with a good hash, which is why `fst_collide` exists.

## Decision and open items

The registry, the lean frames and `walk<bounded>` stay. On both machines and with every compiler tried the builder is faster than the reference at every size and key shape measured, its heap peak is 0.40x to 0.79x of the reference's at every key count, and `get` is unchanged.

Open items, none of them a regression:

- **`intersect` decodes a whole node for each transition.** `range` and `for_each` do not. Fuzzy and regex expansion run in `intersect`. The lean frames of `range` are the candidate; the row to watch is `intersect, 3-letter prefix`.
- **`hash_of` seeds a node with its final flag and then mixes in the final output**, so `mix(0x51, 110) == mix(0x3f, 0)`: a final node whose final output is 110 hashes like the non-final node with the same transitions. `same_node()` keeps the bytes right, and a dictionary build sees no false hit in 662,000 registry probes. Mixing the flag in separately removes the case.
- **`range` with a non-empty upper bound compares the whole path with it at every transition**, so walking a key of L bytes costs O(L^2) when the bound shares a long prefix with it.
- **The first table has 1,024 slots.** A build of one key is 1.2x to 1.5x faster than the reference; a table that starts at 16 to 64 slots would make builds of a few keys cheaper. It is not measured.
- Little-endian hosts only (unchanged).
