# Changelog

Notable changes to fst, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); entries are dated because the
library has no releases.

## 2026-10-31

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
