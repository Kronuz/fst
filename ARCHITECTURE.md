# fst architecture

## Building

The builder keeps the path of the previous key as unfrozen nodes, one per depth.
Inserting a key:

1. Finds the common prefix with the previous key.
2. Freezes every node below that prefix, deepest first. Freezing looks the node up in
   a registry keyed by its signature (final flag, final output, and each transition's
   label, output, and target address); an equal node already written is reused,
   otherwise the node is written and registered. This shares equal suffixes and keeps
   the FST minimal.
3. Pushes outputs down the shared prefix: each transition keeps the minimum of its
   output and the remaining value, and the excess moves to the child's transitions and
   final output.
4. Appends nodes for the new suffix and puts the remaining value on its first
   transition.

`finish()` freezes the last path and writes the root, then the trailer.

## Layout

```
"FST1" | node | node | ... | root node | root address (8) | key count (8)
```

Children precede parents, so every target address is below its parent's and is
stored as a positive delta. The final output, when present, follows the header so a
lookup that ends at a node reads it without skipping the transitions.

- **Chain node:** `[header][final output?][label][output?][delta]`.
- **General node:** `[header][final output?][n?][labels][widths][outputs][deltas]`,
  where outputs and deltas use the smallest byte width that holds the node's largest
  value.

## Lookup

`get` walks the key byte by byte without materializing nodes: a chain node is one
label compare and up to two varints; a general node is an SSE2 search over its label
bytes and two fixed-width loads. Fields are read with 8-byte loads and masked; the
16-byte trailer guarantees those loads stay in the buffer.

## Ordered traversal

`floor` descends along the key, recording each node, then backs up to the deepest
point where a smaller label exists (descending to the maximum key below it) or where
the prefix itself is a key. `range` and `intersect` are depth-first walks in label
order: `range` skips labels below the lower bound while on its path and stops at the
first key at or past the upper bound; `intersect` advances the automaton with each
label and prunes branches where `can_match` is false.
