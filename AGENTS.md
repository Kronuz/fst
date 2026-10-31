# AGENTS.md — fst

## Repo map

```
fst.hh           Format, map (reader), builder, prefix_automaton.
test/test.cc     get/floor/range/intersect against std::map; random alphabets and a dictionary.
test/diff.cc     Byte and traversal equality against the reference test/fst_old.hh.
bench/bench.cc   FST vs open-addressing hash map vs front-coded array: bytes, build, lookup.
bench/cmp_bench.cc  Build and for_each timings against test/fst_old.hh, with an A/A row.
docs/fst.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/fst.d2 docs/fst.svg
```

## Invariants

- The format is fixed: children before parents, positive deltas, final output right after the header.
- Readers over-read up to 16 bytes past a field; the trailer must stay at least 16 bytes.
- The builder requires strictly increasing keys and throws otherwise.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
cmake -S . -B asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" && cmake --build asan && ctest --test-dir asan
build/fst_bench /usr/share/dict/words
```
