// Builder bytes under forced hash collisions. CMake builds this file with FST_TEST_HASH_MASK=0,
// which gives every node the same hash, so each registry probe is decided by same_node()
// alone: a wrong "equal" merges two different nodes and the bytes differ from the reference
// in test/fst_old.hh, whose registry compares whole node signatures.
//
// The key sets are exhaustive over small domains (every subset of the keys of length <= 3 over
// {a,b}, and of length <= 2 over {a,b,c}) under several value shapes, so that nodes differing
// in exactly one of final flag, final output, arc count, label, output or target occur next
// to each other, plus random sets that reach wide nodes and multi-byte outputs and deltas.
#include "fst.hh"
#include "fst_old.hh"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifndef FST_TEST_HASH_MASK
#error "collide.cc must be built with FST_TEST_HASH_MASK"
#endif
static_assert(FST_TEST_HASH_MASK == 0, "every node must hash alike");

static int failures = 0;
static long sets_built = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using entries = std::vector<std::pair<std::string, std::uint64_t>>;

static void compare(const entries& e) {
	fst::builder nb;
	fst_old::builder ob;
	for (const auto& [k, v] : e) { nb.insert(k, v); ob.insert(k, v); }
	const std::string a = nb.finish(), b = ob.finish();
	++sets_built;
	CHECK(a == b);
	if (a != b) { return; }
	fst::map m(a);
	for (const auto& [k, v] : e) { CHECK(m.get(k) == v); }
}

// All keys over the first `letters` letters with length <= max_len, in sorted order.
static std::vector<std::string> universe(unsigned letters, unsigned max_len) {
	std::vector<std::string> keys = {""};
	std::size_t from = 0;
	for (unsigned len = 1; len <= max_len; ++len) {
		const std::size_t to = keys.size();
		for (std::size_t i = from; i < to; ++i) {
			for (unsigned c = 0; c < letters; ++c) { keys.push_back(keys[i] + char('a' + c)); }
		}
		from = to;
	}
	std::sort(keys.begin(), keys.end());
	return keys;
}

static void exhaustive(unsigned letters, unsigned max_len) {
	const std::vector<std::string> keys = universe(letters, max_len);
	std::mt19937_64 rng(letters * 100 + max_len);
	for (std::uint64_t mask = 0; mask < (std::uint64_t(1) << keys.size()); ++mask) {
		entries zero, counter, small, wide;
		std::size_t i = 0;
		for (std::size_t k = 0; k < keys.size(); ++k) {
			if (!(mask >> k & 1)) { continue; }
			zero.emplace_back(keys[k], 0);
			counter.emplace_back(keys[k], i++);
			small.emplace_back(keys[k], rng() % 3);
			wide.emplace_back(keys[k], (rng() & 1) ? rng() >> (rng() % 64) : 300 * (rng() % 3));
		}
		compare(zero);
		compare(counter);
		compare(small);
		compare(wide);
	}
}

int main() {
	exhaustive(2, 3);  // 15 keys, 32768 subsets
	exhaustive(3, 2);  // 13 keys, 8192 subsets

	std::mt19937_64 rng(777);
	const std::vector<std::string> alphabets = {"ab", "abc", "abcdefgh", "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", std::string("\x00\x01\xfe\xff", 4)};
	for (int iter = 0; iter < 400; ++iter) {
		const std::string& al = alphabets[iter % alphabets.size()];
		const std::size_t n = 1 + rng() % (iter % 20 == 0 ? 400 : 60);  // every probe scans all nodes: keep sets small
		const std::size_t maxlen = 1 + rng() % (iter % 20 == 0 ? 40 : 7);
		std::map<std::string, std::uint64_t> ref;
		const int mode = iter % 4;  // counters, small repeated, random wide, zero
		for (std::size_t i = 0; i < n; ++i) {
			std::string k;
			for (std::size_t j = rng() % (maxlen + 1); j > 0; --j) { k.push_back(al[rng() % al.size()]); }
			ref[k] = mode == 0 ? i : mode == 1 ? rng() % 4 : mode == 2 ? rng() >> (rng() % 64) : 0;
		}
		compare(entries(ref.begin(), ref.end()));
	}
	// A node of 40 arcs (general form with the count varint) repeated under different parents.
	for (int variant = 0; variant < 3; ++variant) {
		entries e;
		for (char p = 'a'; p <= 'e'; ++p) {
			for (int c = 0; c < 40; ++c) { e.emplace_back(std::string(1, p) + char('A' + c), variant == 0 ? 0 : variant == 1 ? c : (p == 'c' ? c + 1 : c)); }
		}
		compare(e);
	}
	if (failures) { return 1; }
	std::printf("fst collision tests passed (%ld key sets)\n", sets_built);
}
