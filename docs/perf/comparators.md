# Comparators: method and every row

`fst_bench` builds `fst` and six other structures from one word list, checks that they agree, and times each. The README's table is the medians below, rounded; this page adds the spread, how memory is counted, and the method.

## What is compared

| Name in the tables | What it is | Version |
|---|---|---|
| fst | `fst::builder` and `fst::map` of this repository | the commit that holds this page |
| hash map | the bench's own table: open addressing, linear probing, power-of-two size at least twice the keys, keys in one arena, 24-byte slots (offset, length, tag, value), FNV-1a 64 | `bench/bench.cc` |
| front-coded | the bench's own sorted array in blocks of 16 keys (the first whole, the rest as shared prefix length and suffix), binary search over the block heads, then a scan | `bench/bench.cc` |
| std::map | `std::map<std::string, std::uint64_t, std::less<>>`, inserted in sorted order with `emplace_hint(end())`; lookups pass a `string_view` | libc++ of Apple clang 17 |
| std::unordered_map | `std::unordered_map<std::string, std::uint64_t>` with a transparent `std::hash<std::string_view>`, `reserve(n)` first | libc++ of Apple clang 17 |
| absl::flat_hash_map | `absl::flat_hash_map<std::string, std::uint64_t>`, `reserve(n)` first; found by CMake, not fetched | [Abseil](https://github.com/abseil/abseil-cpp) 20260817.0 (Homebrew) |
| marisa-trie | `marisa::Trie` built from a `marisa::Keyset` with the default configuration; fetched by CMake | [marisa-trie](https://github.com/s-yata/marisa-trie) v0.3.1, commit `3e87d53b78e15f2f43783d5e376561a8c9722051` |
| fst, block index | an `fst` over every 32nd key (7,375 keys); `floor_value` finds the block of a key. Not a competitor: it is the other way the library is used | this repository |

The hash map and the front-coded array are hand-written to be the obvious structure for the job, not tuned.

## Method

- Input: `/usr/share/dict/words`, sorted and deduplicated: 235,976 keys of 9.57 bytes on average. The file is byte-identical on both machines (same MD5 and size), so both used the same keys. The value of a key is its position in the sorted list.
- Build: keys inserted in sorted order; the time is the best of 5 builds, in millions of keys per second. The standard and Abseil tables are reserved for all keys first (a user who knows the count does the same); `std::map` is not reservable. marisa-trie sorts and numbers the keys itself.
- Lookup: 943,904 shuffled present queries (the keys four times over), timed in batches of 1,024; the figure is the median batch in ns per lookup, and the bench keeps the best of 3 interleaved rounds per structure. Queries are independent, so the figure is throughput of one thread, not the latency of a dependent chain.
- Bytes: `fst` is `map.bytes()`, `front-coded` and the bench's `hash map` are their arrays and arena (table slots plus key bytes). `std::map`, `std::unordered_map` and `absl::flat_hash_map` are the heap the container holds after the build, counted as the sum of the allocator's block sizes (`malloc_size`) over every node, bucket array and long-key string, with temporaries already freed (the bench replaces `operator new` and `operator delete` to count). marisa-trie is `io_size()`, its serialized size, which is what it keeps in memory. The library's own bookkeeping is in these numbers; the 24-byte `std::string` header of every key is too, where the container stores strings.
- Agreement: before timing, the bench looks every key up in `fst`, the hash map, the front-coded array, `std::map`, `std::unordered_map` and `absl::flat_hash_map` and aborts with a message unless each returns the key's ordinal; it also checks that each finds none of 33,711 absent keys (every 7th key with a byte `0x01` appended). marisa-trie assigns its own ids, so the bench checks that every key gets an id below 235,976 that no other key has and that `reverse_lookup` of the id returns the key; it finds none of the absent keys either. Every row of both machines ran with these checks on.
- Runs: the bench ran 7 times, one after another, on each machine; the tables give the median of the runs and, in brackets, the smallest and largest. The spread across runs is the A/A figure.

## Machines and commands

- Apple M4 Pro, macOS 26, Apple clang 17.0.0 (clang-1700.3.19.1), `-O3 -DNDEBUG` (CMake Release); load average 6.7 when the runs started.
- Intel Core i9-9980HK (8 cores, 2.4 GHz base), macOS 15, Apple clang 17.0.0 (clang-1700.6.4.2), `-O3 -DNDEBUG`; load average 8.4 when the runs started.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFST_BENCH_COMPARATORS=ON
cmake --build build --target fst_bench
build/fst_bench /usr/share/dict/words
```

`FST_BENCH_COMPARATORS` needs CMake 3.22 (marisa-trie's), Abseil with its CMake package (Homebrew `abseil`) and network access to fetch marisa-trie. It is off by default and the `fst` target never depends on it; without it the bench times the first five rows (`fst`, the two hand-written structures, `std::map` and `std::unordered_map`).

## Rows

### Apple M4 Pro (7 runs)

| Structure | Bytes | Bytes per key | Build (Mkeys/s) | Lookup p50 (ns) |
|---|---:|---:|---:|---:|
| fst | 1,464,226 | 6.20 | 10.42 (9.95 to 10.85) | 152.1 (150.4 to 155.4) |
| hash map | 14,840,821 | 62.89 | 46.29 (40.56 to 48.01) | 27.5 (23.6 to 29.2) |
| front-coded | 1,400,584 | 5.94 | 47.65 (46.43 to 48.06) | 253.9 (251.0 to 255.4) |
| std::map | 15,103,232 | 64.00 | 5.89 (5.73 to 6.42) | 422.2 (342.7 to 434.0) |
| std::unordered_map | 13,228,176 | 56.06 | 28.42 (26.71 to 30.71) | 36.3 (34.9 to 37.2) |
| absl::flat_hash_map | 17,302,256 | 73.32 | 110.17 (98.77 to 114.09) | 17.5 (17.2 to 19.0) |
| marisa-trie | 743,368 | 3.15 | 4.68 (4.63 to 4.73) | 330.7 (328.8 to 331.8) |
| fst, block index | 84,271 | 0.36 |  | 112.1 (110.1 to 113.5) |

### Intel Core i9-9980HK (7 runs)

| Structure | Bytes | Bytes per key | Build (Mkeys/s) | Lookup p50 (ns) |
|---|---:|---:|---:|---:|
| fst | 1,464,226 | 6.20 | 5.87 (5.63 to 6.09) | 144.7 (143.4 to 170.0) |
| hash map | 14,840,821 | 62.89 | 19.15 (16.56 to 20.60) | 83.9 (80.0 to 91.8) |
| front-coded | 1,400,584 | 5.94 | 20.86 (19.73 to 21.28) | 530.3 (509.7 to 607.3) |
| std::map | 15,103,232 | 64.00 | 6.22 (6.15 to 6.83) | 491.9 (475.4 to 517.4) |
| std::unordered_map | 13,228,176 | 56.06 | 8.54 (7.70 to 8.81) | 150.3 (149.1 to 158.3) |
| absl::flat_hash_map | 17,302,256 | 73.32 | 31.91 (28.48 to 33.00) | 68.5 (67.2 to 70.6) |
| marisa-trie | 743,368 | 3.15 | 2.21 (2.11 to 2.36) | 460.3 (438.4 to 491.3) |
| fst, block index | 84,271 | 0.36 |  | 148.1 (145.2 to 149.5) |

