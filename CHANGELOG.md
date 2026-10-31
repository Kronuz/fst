# Changelog

Notable changes to fst, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); entries are dated because the
library has no releases.

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
