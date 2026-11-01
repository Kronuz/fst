# AGENTS.md — fst

## Repo map

```
fst.hh           Format, map (reader), builder, prefix_automaton.
test/test.cc     get/floor/range/intersect against std::map; random alphabets and a dictionary.
test/diff.cc     Byte and traversal equality against the reference test/fst_old.hh. Also built as
                 fst_diff_wide with FST_TEST_NARROW_LIMIT=70000, which moves the registry to 16-byte slots early.
test/collide.cc  Builder bytes against the reference with FST_TEST_HASH_MASK=0 (every node hashes alike), so
                 same_node() decides every registry hit; exhaustive over small key sets plus random sets.
test/memory.cc   Peak heap (counted through operator new) of the builder against the reference builder over a
                 sweep of key counts.
bench/bench.cc   FST vs open-addressing hash map vs front-coded array: bytes, build, lookup.
bench/cmp_bench.cc  Build and for_each timings against test/fst_old.hh over a word list, then edge rows (builds from 1 to
                 300,000 keys, decimal, random and 200-byte keys, range, intersect, get), all with an A/A row.
bench/build_rss.cc  Peak RSS of one build, the builder against the reference, one process per measurement (fst_build_rss).
tools/mutants.py  Breaks same_node(), the registry's slot switch and the for_each walk in 21 ways and requires the tests
                 to fail on each.
docs/perf/builder.md  The registry and the traversal, measured: method, tables, variants not taken, open items.
docs/fst.d2     README diagram; render: d2 --layout elk --sketch --theme 302 docs/fst.d2 docs/fst.svg
```

## Invariants

- The format is fixed: children before parents, positive deltas, final output right after the header.
- Readers over-read up to 16 bytes past a field; the trailer must stay at least 16 bytes.
- The builder requires strictly increasing keys and throws otherwise.
- The builder's output must not depend on the registry: bytes are identical for any hash function and slot
  width. `FST_TEST_HASH_MASK` (masks every node hash) and `FST_TEST_NARROW_LIMIT` (the output size at which
  8-byte slots give way to 16-byte ones) exist only for the tests above; never define them in production.

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
cmake -S . -B asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" && cmake --build asan && ctest --test-dir asan
python3 tools/mutants.py    # after touching same_node(), the registry or the walk
build/fst_bench /usr/share/dict/words
```
