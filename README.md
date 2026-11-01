# fst

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/fst/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/fst/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Header-only minimal finite state transducers for C++20: a compact ordered map from byte strings to uint64, read in place.
The builder shares common prefixes and suffixes as it goes, so the system
dictionary takes about 6 bytes per key, and the reader works on the bytes where
they are: a string or a memory-mapped file. Reach for it for term dictionaries
and vocabularies, large static key sets that need exact, floor, range, prefix,
or fuzzy lookups.

![fst: "in", "on" and "tin" share their last state; each key's value is the sum of the outputs on its path](docs/fst.svg)

## ✨ Highlights

- **Incremental minimization.** `fst::builder` takes keys in strictly increasing
  byte order and shares equal suffixes as nodes freeze (Daciuk et al., 2000);
  values move toward the root, so a key's value is the sum of the outputs on its
  path.
- **Read in place.** `fst::map` reads a `std::string_view` or `std::span` without
  copying; the constructor validates every node once, and the `fst::trusted`
  overload skips that for bytes already checked.
- **Ordered lookups.** `get`, `contains`, `floor` (the largest key `<=` a key,
  with that key), `floor_value` (its value alone, with no allocation),
  `range(lo, hi, f)`, and `for_each`.
- **Automaton intersection.** `intersect(a, f)` visits the keys a deterministic
  automaton accepts and prunes every branch it rejects. `prefix_automaton` ships;
  any type with `start`, `accept`, `is_match`, and `can_match` works, such as a
  wildcard or Levenshtein automaton.
- **Deep keys are safe.** Walks are iterative, so a long key costs heap, not
  native stack.
- **One header, no dependencies.** `fst.hh`; label search uses SSE2 on x86-64.

## 🚀 Quick start

```cpp
#include "fst.hh"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

int main() {
	fst::builder b;  // keys in strictly increasing byte order
	b.insert("cat", 7);
	b.insert("cats", 8);
	b.insert("dog", 3);
	std::string bytes = b.finish();  // store it, map it, send it

	fst::map m(bytes);  // validates, then reads in place
	std::printf("%llu\n", (unsigned long long)*m.get("cats"));  // 8
	auto [key, value] = *m.floor("cow");
	std::printf("%s %llu\n", key.c_str(), (unsigned long long)value);  // cats 8

	auto print = [](std::string_view k, std::uint64_t v) {
		std::printf("%.*s=%llu ", int(k.size()), k.data(), (unsigned long long)v);
		return true;  // false stops the walk
	};
	m.range("c", "d", print);  // cat=7 cats=8
	m.intersect(fst::prefix_automaton{"do"}, print);  // dog=3
	std::printf("\n");
}
```

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; no dependencies. CI builds it
on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan,
and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(fst
  GIT_REPOSITORY https://github.com/Kronuz/fst.git
  GIT_TAG        aa34be19001777b9722a4c6619aabe2b3f8247ed)  # or main for the tip
FetchContent_MakeAvailable(fst)

target_link_libraries(my_app PRIVATE fst::fst)
```

```cpp
#include "fst.hh"
```

## 🎯 When to use it

Use it for term dictionaries, vocabularies, and block indexes: large, static,
sorted key sets read far more often than they are built.

Not for:

- **Keys that change.** There is no insert or delete after `finish()`; rebuild
  instead.
- **Unsorted input.** The builder requires strictly increasing keys and throws
  otherwise.
- **Raw lookup speed.** A hash map looks up about 5 times faster when order,
  prefixes, and size do not matter (see Performance).
- **Values other than `uint64_t`.** Store an index into your own table.
- **Big-endian hosts.** The format and the reader assume little-endian.

## ⚡ Performance

`bench/bench.cc` builds each structure from `/usr/share/dict/words` (235,976
distinct keys of 9.57 bytes on average, the same file on both machines) and maps
each key to its ordinal in sorted order, then looks up shuffled present keys one
at a time. Bytes per key is the structure's own memory over the keys; Build is
millions of keys per second from sorted input (higher is faster); Lookup is the
median nanoseconds of one `get` (lower is faster). The rows: `fst`; `Hash map`
(open addressing, linear probing, keys in one arena) and `Front-coded array`
(blocks of 16 keys, binary search over the block heads), which are the bench's
own hand-written reference structures; `std::map` and `std::unordered_map` of
the C++ standard library (libc++); `absl::flat_hash_map` of
[Abseil](https://github.com/abseil/abseil-cpp) 20260817.0;
[marisa-trie](https://github.com/s-yata/marisa-trie) v0.3.1, a compact static
trie; and `fst, block index`, which is not a competitor but a use: an `fst` over
every 32nd key, where `floor_value` finds the block a key lives in (bytes per
key are over all keys). Every structure returns the same ordinal for every key
and nothing for 33,711 absent keys, and the bench aborts with a message on a
mismatch; marisa-trie numbers the keys itself, so the bench checks that its ids
are distinct and that `reverse_lookup` returns each key.

Apple M4 Pro (macOS 26) and Intel Core i9-9980HK (macOS 15), Apple clang 17,
CMake `Release` (`-O3 -DNDEBUG`), median of 7 runs:

| Structure | Bytes per key | Build, M4 Pro (Mkeys/s) | Lookup, M4 Pro (ns) | Build, i9 (Mkeys/s) | Lookup, i9 (ns) |
|---|---:|---:|---:|---:|---:|
| fst | 6.20 | 10.4 | 152 | 5.9 | 145 |
| Hash map | 62.89 | 46.3 | 28 | 19.1 | 84 |
| Front-coded array | 5.94 | 47.6 | 254 | 20.9 | 530 |
| `std::map` | 64.00 | 5.9 | 422 | 6.2 | 492 |
| `std::unordered_map` | 56.06 | 28.4 | 36 | 8.5 | 150 |
| `absl::flat_hash_map` | 73.32 | 110.2 | 18 | 31.9 | 68 |
| marisa-trie | 3.15 | 4.7 | 331 | 2.2 | 460 |
| fst, block index | 0.36 |  | 112 |  | 148 |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFST_BENCH_COMPARATORS=ON
cmake --build build --target fst_bench
build/fst_bench /usr/share/dict/words
```

`fst` answers range, `floor_value` and prefix (automaton intersection) queries
over its sorted keys; `std::map` can also walk its keys in order and marisa-trie
answers prefix searches, but the hash tables answer point lookups only. `fst` pays
for that in point lookups: the hash tables are faster on both machines
(`absl::flat_hash_map` 8.7 times faster on the M4 Pro and 2.1 times on the i9,
the bench's `Hash map` 5.5 and 1.7, `std::unordered_map` 4.2 times on the M4 Pro
and level on the i9), at 56 to 73 bytes per key against 6.20. `fst` looks up 2.8
to 3.4 times faster than `std::map` and 2.2 to 3.2 times faster than
marisa-trie, which is half its size (3.15 bytes per key) and builds 2.2 to 2.7
times slower. [docs/perf/comparators.md](docs/perf/comparators.md) has the
method, how bytes are counted, the spread of the runs and every row;
[docs/perf/builder.md](docs/perf/builder.md) measures the builder's time and
memory.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): the builder, the byte layout, lookup, and
  the ordered walks.
- [docs/perf/comparators.md](docs/perf/comparators.md): the lookup benchmark's
  method and every row for both machines.
- [docs/perf/builder.md](docs/perf/builder.md): the builder's registry and the
  traversal, measured.
- [CHANGELOG.md](CHANGELOG.md): what changed.
- [AGENTS.md](AGENTS.md): repository map, invariants, and how to verify a
  change.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#fst)
  in Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
python3 tools/mutants.py
```

The tests check `get`, `floor`, `range`, and prefix intersection against
`std::map` on random key sets and the system dictionary, and the builder's bytes
against a reference implementation; `tools/mutants.py` breaks `fst.hh` in 21
ways and requires the tests to fail on each. `-DFST_FUZZ=ON` with clang builds the libFuzzer target `fst_fuzz`.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
