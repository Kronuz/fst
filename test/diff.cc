// Differential test against the reference implementation in test/fst_old.hh: the builder
// must emit identical bytes and range/for_each must visit identical (key, value)
// sequences, over many random key sets and edge cases.
#include "fst.hh"
#include "fst_old.hh"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using reference = std::map<std::string, std::uint64_t>;
using entries = std::vector<std::pair<std::string, std::uint64_t>>;

static void compare(const reference& ref, std::mt19937_64& rng, const std::string& alphabet) {
	fst::builder nb;
	fst_old::builder ob;
	for (const auto& [k, v] : ref) { nb.insert(k, v); ob.insert(k, v); }
	std::string a = nb.finish(), b = ob.finish();
	CHECK(a == b);
	if (a != b) { return; }
	fst::map nm(a);
	fst_old::map om(b);
	auto collect = [](auto& m, std::string_view lo, std::string_view hi, std::size_t limit) {
		entries e;
		m.range(lo, hi, [&](std::string_view k, std::uint64_t v) { e.emplace_back(std::string(k), v); return e.size() < limit; });
		return e;
	};
	auto random_key = [&] {
		std::string k;
		std::size_t n = rng() % 8;
		for (std::size_t i = 0; i < n; ++i) { k.push_back(alphabet[rng() % alphabet.size()]); }
		return k;
	};
	CHECK(collect(nm, {}, {}, ~std::size_t(0)) == collect(om, {}, {}, ~std::size_t(0)));
	for (int i = 0; i < 40; ++i) {
		std::string lo = random_key(), hi = random_key();
		std::size_t limit = (rng() % 3 == 0) ? rng() % 5 + 1 : ~std::size_t(0);
		CHECK(collect(nm, lo, hi, limit) == collect(om, lo, hi, limit));
		CHECK(collect(nm, lo, {}, limit) == collect(om, lo, {}, limit));
	}
}

static std::string rand_key(std::mt19937_64& rng, const std::string& alphabet, std::size_t maxlen) {
	std::string k;
	std::size_t n = rng() % (maxlen + 1);
	for (std::size_t i = 0; i < n; ++i) { k.push_back(alphabet[rng() % alphabet.size()]); }
	return k;
}

int main() {
	std::mt19937_64 rng(12345);
	// Edge cases.
	{
		std::mt19937_64 r(1);
		compare({}, r, "ab");
		compare({{"", 0}}, r, "ab");
		compare({{"", 7}, {"a", 7}, {"ab", 9}}, r, "ab");
		compare({{"a", 0}}, r, "ab");
		compare({{"a", ~0ull}, {"b", 0}, {"c", ~0ull}}, r, "abc");
	}
	const std::vector<std::string> alphabets = {"ab", "abc", "abcdefgh", "abcdefghijklmnopqrstuvwxyz", std::string("\x00\x01\xfe\xff", 4)};
	for (int iter = 0; iter < 600; ++iter) {
		const std::string& al = alphabets[iter % alphabets.size()];
		std::size_t n = rng() % (iter % 7 == 0 ? 3000 : 200);
		std::size_t maxlen = 1 + rng() % 12;
		reference ref;
		int mode = iter % 4;  // value shapes: counters, small repeated, random wide, zero
		for (std::size_t i = 0; i < n; ++i) {
			std::uint64_t v = mode == 0 ? i : mode == 1 ? rng() % 4 : mode == 2 ? rng() >> (rng() % 64) : 0;
			ref[rand_key(rng, al, maxlen)] = v;
		}
		compare(ref, rng, al);
	}
	// Wide nodes (>= 31 arcs) and deep keys.
	for (int iter = 0; iter < 20; ++iter) {
		reference ref;
		for (int i = 0; i < 2000; ++i) { ref[rand_key(rng, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 5)] = rng() % 100; }
		ref[std::string(500, 'x')] = 1;
		ref[std::string(500, 'x') + "y"] = 2;
		compare(ref, rng, "abcxyzAZ09");
	}
	// Dictionary, if present.
	if (std::ifstream in("/usr/share/dict/words"); in) {
		reference ref;
		std::uint64_t i = 0;
		for (std::string l; std::getline(in, l);) { ref[l] = i++; }
		compare(ref, rng, "abcdefghijklmnopqrstuvwxyz'");
	}
	if (failures) { return 1; }
	std::puts("all fst differential tests passed");
}
