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
keys, 9.57 bytes each) and looks up shuffled present keys; values are key
ordinals. The hash map (open addressing, linear probing, keys in one arena) and
the front-coded array (blocks of 16) are the bench's own reference structures.
Apple M4 Pro, Apple clang 17, `-O3 -DNDEBUG`, the mean of two runs:

| Structure | Bytes per key | Build (Mkeys/s) | Lookup p50 |
|---|---:|---:|---:|
| fst | 6.20 | 9.1 | 154 ns |
| Hash map | 62.89 | 33.3 | 29 ns |
| Front-coded array | 5.94 | 42.8 | 266 ns |
| fst over every 32nd key, `floor_value` | 0.36 (84 KB total) | | 111 ns |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target fst_bench
build/fst_bench /usr/share/dict/words
```

The last row is a block index: an FST over every 32nd key finds a key's block
with `floor_value`. [docs/perf/builder.md](docs/perf/builder.md) measures the
builder's time and memory.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): the builder, the byte layout, lookup, and
  the ordered walks.
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
