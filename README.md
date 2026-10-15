# fst

[![CI](https://github.com/Kronuz/fst/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/fst/actions/workflows/ci.yml)

Minimal acyclic finite state transducers for C++20: an ordered map from byte strings
to `uint64` values that shares common prefixes and suffixes, so a vocabulary takes a
few bytes per key. Header-only, no dependencies. Built from sorted keys, read in
place from bytes (a string, a memory-mapped file).

## What it is

One header, `fst.hh`:

- `fst::builder` takes keys in strictly increasing byte order with a value each and
  produces the FST bytes. Minimization is incremental (Daciuk et al., 2000): equal
  suffixes are shared as they are frozen, and values are pushed toward the root, so a
  key's value is the sum of the outputs on its path.
- `fst::map` reads those bytes without copying or parsing them up front:
  - `get(key)` and `contains(key)`
  - `floor(key)`: the largest key `<=` a key, with that key; `floor_value(key)`: just
    its value, in one allocation-free pass, the lookup a block-tree term dictionary
    uses to find a key's block
  - `range(lo, hi, f)`: keys in `[lo, hi)` in ascending order
  - `intersect(automaton, f)`: keys a deterministic automaton accepts, pruning every
    branch the automaton rejects (prefix, wildcard, and Levenshtein matching)

## Usage

```cpp
#include "fst.hh"

fst::builder b;
b.insert("cat", 7);
b.insert("cats", 8);
b.insert("dog", 3);
std::string bytes = b.finish();

fst::map m(bytes);
m.get("cats");                  // 8
m.floor("cow");                 // {"cats", 8}
m.range("c", "d", [](std::string_view k, std::uint64_t v) { return true; });
m.intersect(fst::prefix_automaton{"ca"}, [](std::string_view k, std::uint64_t v) { return true; });
```

An automaton is any type with `start()`, `accept(state, byte)`, `is_match(state)`,
and `can_match(state)`.

## Performance

`bench/bench.cc` compares the FST with an open-addressing hash map (linear probing,
keys in one arena) and front coding in blocks of 16; values are key ordinals and
lookups use shuffled present keys.

English Wikipedia vocabulary (3,964,752 keys, 8.52 bytes each), Linux VM on an Intel
i9-9980HK, Clang 18 `-O3`:

| Structure | Bytes per key | Build (Mkeys/s) | Lookup p50 |
|---|---:|---:|---:|
| fst | 5.91 | 1.12 | 668 ns |
| Hash map | 59.30 | 6.04 | 275 ns |
| Front-coded array | 5.12 | 29.42 | 1,452 ns |
| fst over every 32nd key, `floor_value` | 0.29 (1.16 MB total) | | 447 ns |

At this size every structure is bound by memory latency: the 23 MB FST does not fit
in the last-level cache, so a lookup takes a few cache misses at branching nodes
against the hash map's one or two. The FST is a tenth of the hash map's size. Used as
the index over term blocks, the FST is small enough to stay cached and `floor_value`
runs at 1.63× the hash map's exact lookup.

`/usr/share/dict/words` (235,976 keys) on macOS, Apple clang: the FST takes 6.20
bytes per key and looks up in 224 ns against the hash map's 112 ns.

## Format

Nodes are written children first; transitions store the distance down to their
target. A node with one transition (a "chain", most of a vocabulary's suffixes) is a
header, a label, an optional output varint, and a delta varint. Other nodes store
their labels as one ascending byte array, searched 16 at a time with SSE2, followed
by fixed-width output and target columns. A 16-byte trailer holds the root address
and the key count, and also lets readers load whole words past any field.
See [ARCHITECTURE.md](ARCHITECTURE.md).

## Build and test

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
build/fst_bench KEYS_FILE
```

The tests compare `get`, `floor`, `range`, and prefix intersection with `std::map`
on random key sets over 2-, 8-, and 256-letter alphabets (including the empty key
and zero outputs) and on the system dictionary.

## Limits

- Keys must arrive sorted; there is no update or delete. Rebuild to change it.
- Values are unsigned and additive.
- Little-endian hosts only.

## License

MIT. See [LICENSE](LICENSE).
