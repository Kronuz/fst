# Changelog

Notable changes to fst, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); entries are dated because the
library has no releases.

## 2026-11-01

### Added

- `bench/bench.cc` times `std::map`, `std::unordered_map`, `absl::flat_hash_map` (found) and
  marisa-trie v0.3.1 (fetched) beside `fst` and the bench's own hash map and front-coded
  array, counts each structure's heap bytes, and aborts when any structure returns a
  different ordinal for a key. The last two are built only with `-DFST_BENCH_COMPARATORS=ON`
  and are not dependencies of the library. `docs/perf/comparators.md` has the method and every
  row on the Apple M4 Pro and the Intel Core i9-9980HK.
- `bench/build_rss.cc` (`fst_build_rss`): the peak resident set of a process that builds one
  FST from N random 12-letter keys, with the builder and with the reference builder, one
  process per measurement, and a check that both produce the same bytes. `fst_memory` counts
  the heap and fails on a regression; this reports the resident set the README and the
  2026-10-31 entry quote, which was measured by a probe that was not in the repository. The
  audit's finding E3: peak build memory had no bench.
- `bench/cmp_bench.cc`: edge rows after the word-list rows, each against the reference with an
  A/A row: builds of 1 to 4,096 random keys, of 300,000 random 12-letter keys, of 300,000
  decimal keys and of 20,000 keys of 200 bytes; `for_each` on the last three and on 200,000
  random keys; `range` from a 3-letter prefix returning 1, 10 and 100 keys; prefix `intersect`;
  `get`. They cover shapes the word list does not (the registry's first table dominating a
  small build, long chains, long keys, short `range` calls) and are the rows the audit timed
  by hand. `fst_cmp_bench KEYS_FILE ROWS` runs the edge rows whose group name contains ROWS,
  and runs them alone when KEYS_FILE has no keys.
- `tools/mutants.py`: breaks `same_node()` (11 mutants), the registry's switch to 16-byte
  slots (6) and the `for_each` walk (4) and requires the tests to fail on each. The mutants
  quoted below were run by hand until now.
- `docs/perf/builder.md`: the registry and the traversal, measured: method, memory by key count,
  the edge rows on the M4 and the i9 with three compilers, the variants that were not taken,
  the proof and the open items.

### Documentation

- The `for_each` table of 2026-10-31 labelled one row "200k random 12-letter keys"; its keys
  have 1 to 12 letters. The label is corrected.
- The 2026-10-31 entry for `test/memory.cc` counted the sizes the previous registry fails at
  as 6 of 15, up to 1.48x. Run against the header of `7376bd9` it fails at 8 of 15 on the M4
  with libc++, up to 1.44x; the entry states that.
- The README quotes the resident set that `fst_build_rss` prints (156 MiB for 1 million keys,
  24 MiB of it the sorted keys). The 2026-10-31 table was measured by a probe that grew its key
  vector by `push_back`; its numbers differ from the bench's by 2% to 12%.

## 2026-10-31

### Changed

- The builder's node registry stores 8-byte slots (the low 32 bits of the node hash and a
  32-bit address) in a table kept at most half full, starting at 1,024 slots, and moves to
  16-byte slots when the output nears 4 GiB. Before, a slot held a 64-bit hash and a 64-bit
  address (16 bytes), so the table's memory per node swung between about 32 and 64 bytes as
  it doubled, with the old and the new table alive together while it grew. Peak RSS of the
  process for one build of random 12-letter keys, the sorted key array included (Apple M4
  Pro; "reference" is the signature-string registry in `test/fst_old.hh`, "before" is
  `7376bd9`):

  | keys | reference (MiB) | before (MiB) | now (MiB) |
  |---:|---:|---:|---:|
  | 100,000 | 47.5 | 38.2 | 25.9 |
  | 300,000 | 108.0 | 144.5 | 80.5 |
  | 1,000,000 | 335.5 | 294.9 | 164.5 |
  | 2,000,000 | 625.6 | 582.4 | 323.4 |
  | 3,000,000 | 769.6 | 1130.3 | 620.8 |

  Before, 300k keys needed 1.34x and 3M keys 1.47x the reference's peak; now no size needs
  more than 0.81x of it. A build of 1 to 16 keys is faster too, because the first table is
  8 KiB instead of 16 KiB. Output bytes are identical. Times in ns per key, one binary per
  version (reference is `12657aa`), best of 3 alternating runs on the M4 and of 2 on the
  i9, on an otherwise idle host; the control run of the previous version against itself agrees
  within 3%:

  Apple M4 Pro, Apple clang 17:

  | row (ns per key) | reference | before | now | before / now |
  |---|---:|---:|---:|---:|
  | build, 1 keys | 337 | 381 | 295 | 1.29x |
  | build, 16 keys | 367 | 130 | 118 | 1.10x |
  | build, 64 keys | 331 | 85.4 | 85.1 | 1.00x |
  | build, 256 keys | 337 | 83.9 | 80.2 | 1.05x |
  | build, 1024 keys | 327 | 84.2 | 82.7 | 1.02x |
  | build, 4096 keys | 317 | 97.7 | 95.4 | 1.02x |
  | build, `/usr/share/dict/words` (236k keys) | 242 | 101 | 102 | 0.99x |
  | build, 300k random 12-letter keys | 1722 | 396 | 338 | 1.17x |
  | build, 300k decimal keys | 67.3 | 34.6 | 34.6 | 1.00x |
  | build, 20k keys of 200 bytes | 82440 | 9719 | 8796 | 1.10x |
  | `for_each`, dictionary | 51.9 | 36.2 | 35.5 | 1.02x |
  | `for_each`, 300k decimal keys | 9.1 | 5.2 | 5.1 | 1.03x |
  | `get`, dictionary | 93.8 | 92.7 | 92.7 | 1.00x |

  Intel i9-9980HK, Apple clang 17:

  | row (ns per key) | reference | before | now | before / now |
  |---|---:|---:|---:|---:|
  | build, 1 keys | 1002 | 724 | 657 | 1.10x |
  | build, 16 keys | 1085 | 269 | 261 | 1.03x |
  | build, 64 keys | 941 | 180 | 178 | 1.01x |
  | build, 256 keys | 924 | 185 | 176 | 1.05x |
  | build, 1024 keys | 873 | 229 | 211 | 1.09x |
  | build, 4096 keys | 807 | 225 | 207 | 1.09x |
  | build, `/usr/share/dict/words` (236k keys) | 582 | 202 | 176 | 1.15x |
  | build, 300k random 12-letter keys | 3702 | 963 | 775 | 1.24x |
  | build, 300k decimal keys | 160 | 67.9 | 63.4 | 1.07x |
  | build, 20k keys of 200 bytes | 140739 | 28454 | 22512 | 1.26x |
  | `for_each`, dictionary | 75.3 | 63.8 | 65.3 | 0.98x |
  | `for_each`, 300k decimal keys | 14.3 | 13.2 | 13.2 | 0.99x |
  | `get`, dictionary | 131 | 126 | 128 | 0.99x |

  Intel i9-9980HK, Homebrew LLVM 23:

  | row (ns per key) | reference | before | now | before / now |
  |---|---:|---:|---:|---:|
  | build, 1 keys | 991 | 745 | 668 | 1.12x |
  | build, 16 keys | 1095 | 283 | 272 | 1.04x |
  | build, 64 keys | 925 | 191 | 183 | 1.04x |
  | build, 256 keys | 925 | 194 | 182 | 1.07x |
  | build, 1024 keys | 884 | 232 | 215 | 1.08x |
  | build, 4096 keys | 819 | 228 | 206 | 1.10x |
  | build, `/usr/share/dict/words` (236k keys) | 578 | 202 | 170 | 1.19x |
  | build, 300k random 12-letter keys | 3748 | 957 | 738 | 1.30x |
  | build, 300k decimal keys | 158 | 63.9 | 59.5 | 1.07x |
  | build, 20k keys of 200 bytes | 140650 | 28155 | 23240 | 1.21x |
  | `for_each`, dictionary | 59.7 | 63.7 | 61.6 | 1.03x |
  | `for_each`, 300k decimal keys | 10.1 | 13.2 | 13.4 | 0.98x |
  | `get`, dictionary | 127 | 126 | 126 | 1.00x |

  The audit's finding E3. (`aa3aa3a`)

- `for_each` walks with the bound tests compiled out: `range` and `for_each` share one
  template, `walk<bounded>`, and `for_each` instantiates it without the lower and upper
  bound checks, the on-path flag and the skip of labels below the lower bound. Visit order
  and results are unchanged. With LLVM on x86-64 the `range` of the previous change was
  slower than the original code on `for_each` (Intel i9-9980HK, LLVM 23: 0.68x to 0.85x
  of the reference on four key sets); it is now 1.06x to 1.25x, and 1.2x to 1.3x with Apple
  clang 17 (was 1.1x). ns per key, one binary per version, best of 3 alternating runs on an
  otherwise idle host, "reference" is `12657aa` and "before" is `aa3aa3a`:

  | row | i9 LLVM 23: reference / before / now | i9 Apple clang 17 | M4 Apple clang 17 |
  |---|---:|---:|---:|
  | dictionary (236k keys) | 53.2 / 62.6 / 49.4 | 70.5 / 63.6 / 58.5 | 57.4 / 36.7 / 34.8 |
  | 300k decimal keys | 8.9 / 13.2 / 7.1 | 14.6 / 12.8 / 11.3 | 9.1 / 5.0 / 4.5 |
  | 200k random keys of 1 to 12 letters | 63.3 / 76.2 / 59.6 | 90.5 / 78.4 / 72.4 | 68.9 / 41.7 / 40.4 |
  | 20k keys of 200 bytes | 1643 / 2275 / 1499 | 2434 / 2059 / 1881 | 1986 / 1099 / 1088 |
  | `range`, first 10 keys of 200k (ns per call) | 1340 / 993 / 1050 | 1399 / 1078 / 1098 | 826 / 497 / 496 |

  The audit's finding E5, which it measured on `docset`; the same effect showed in `fst`
  on the x86-64 LLVM rows. (`fe22b32`)

### Added

- `test/diff.cc` compares `for_each` (it covered `range` only) with the reference, with
  and without an early stop, over every key set it builds. A mutant that makes
  `for_each` emit no keys, one that ignores the stop, one that drops the transition output
  and one that never advances fail it.
- `test/collide.cc` (`fst_collide`): the builder under `FST_TEST_HASH_MASK=0`, a test-only
  macro that gives every node the same hash, so that `same_node()` alone decides every
  registry hit. It compares the builder's bytes with the reference over every subset of the
  keys of length <= 3 over {a,b} and <= 2 over {a,b,c} under four value shapes, plus random
  sets with wide nodes and multi-byte outputs and deltas. Eleven mutants of `same_node()`
  (each comparison removed, and "every hit is equal") all fail it; `fst_diff` and `fst_test`
  let ten and nine of them through. The audit's finding E2. (`9af3f3f`)
- `test/memory.cc` (`fst_memory`): the builder's peak heap, counted through `operator new`,
  against the reference builder's over 15 key counts from 1,000 to 2 million; it fails for
  the previous registry (`7376bd9`) at 8 of the 15 on the M4 with libc++, up to 1.44x at
  5,000 keys.
- `fst_diff_wide`: `test/diff.cc` with `FST_TEST_NARROW_LIMIT=70000`, a test-only macro that
  makes the registry switch to 16-byte slots at 70 KB, during the larger sets, so the switch
  and the wide table run against the reference. Six mutants of the switch fail it.

### Removed

- The `ubsan/` build directory committed in `b08055f` (68 files) and the object files and
  binaries in it; `.gitignore` lists `ubsan/`, `asan/`, `tsan/` and `cmake-build-*/` next to
  `build*/`. The files stay in the history. The audit's finding E1. (`847aa0f`)

### Documentation

- The README performance table is measured on an Apple M4 Pro with
  `/usr/share/dict/words` at `c57e01f`. It replaces a table measured on 2026-10-22 at
  `12657aa`, before the builder and `range` speedups below, on a Linux VM (Intel
  i9-9980HK, Clang 18 `-O3`) with an English Wikipedia vocabulary (3,964,752 keys,
  8.52 bytes each):

  | Structure | Bytes per key | Build (Mkeys/s) | Lookup p50 |
  |---|---:|---:|---:|
  | fst | 5.91 | 1.12 | 668 ns |
  | Hash map | 59.30 | 6.04 | 275 ns |
  | Front-coded array | 5.12 | 29.42 | 1,452 ns |
  | fst over every 32nd key, `floor_value` | 0.29 (1.16 MB total) | | 447 ns |

  At that size the 23 MB FST did not fit in the last-level cache, and `floor_value`
  ran at 1.63x the hash map's exact lookup. On `/usr/share/dict/words` (macOS, Apple
  clang) the FST took 6.20 bytes per key and looked up in 224 ns against the
  hash map's 112 ns. The Wikipedia key file is not available on the M4 host, so the
  large-vocabulary row is not re-measured.

## 2026-10-30

### Changed

- `range` (and `for_each`) walks with lean frames: a frame holds node pointers and a
  transition cursor, decodes a transition only when it is taken, and writes the key into
  a preallocated buffer. Before, `range` built a full node struct per transition, copied
  it onto the stack, and pushed and popped the key a byte at a time. Visit order and
  results are unchanged. `for_each` on `/usr/share/dict/words` (236k keys, arm64,
  `build/fst_cmp_bench`, A/A 1.0): 55 -> 37 ns/key. (`b08055f`)
- The builder's node registry is a flat open-addressing table of (64-bit hash, address)
  whose hits are verified against the node bytes already in the output, and pending
  nodes are cleared in place. Before, it owned one heap `std::string` signature per
  written node and reset each pending node with `pending{}`, freeing its arc vector.
  Output bytes are identical. Build on the same words: 220 -> 93 ns/key. (`b1e8ff3`)

### Added

- `test/diff.cc` and `test/fst_old.hh`: a differential test against the reference
  implementation (identical builder bytes, identical `range`/`for_each` sequences over
  random key sets).
- `bench/cmp_bench.cc` (`fst_cmp_bench`): build and `for_each` timings against the
  reference, with an A/A row for the noise floor.

### Fixed

- Verifying a registry hit read node fields with the reader's 8-byte load over the
  output still being built, which has no trailer yet, so a hit near the end read past
  the buffer (ASan/TSan heap-use-after-free). It reads exact bytes. (`8058003`)
