// fst: get, floor, range, and automaton intersection against std::map on random and
// dictionary key sets, including the empty key, shared suffixes, and wide nodes.
#include "fst.hh"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <random>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

using reference = std::map<std::string, std::uint64_t>;

static std::string build(const reference& ref) {
	fst::builder b;
	for (const auto& [k, v] : ref) { b.insert(k, v); }
	return b.finish();
}

static void check(const reference& ref, std::mt19937_64& rng, const std::string& alphabet, int probes = 300) {
	std::string bytes = build(ref);
	fst::map m(bytes);
	CHECK(m.size() == ref.size());

	for (const auto& [k, v] : ref) { CHECK(m.get(k) == v); }

	auto random_key = [&] {
		std::string k;
		std::size_t n = rng() % 8;
		for (std::size_t i = 0; i < n; ++i) { k.push_back(alphabet[rng() % alphabet.size()]); }
		return k;
	};

	for (int i = 0; i < probes; ++i) {
		std::string probe = random_key();
		auto it = ref.find(probe);
		CHECK(m.get(probe) == (it == ref.end() ? std::nullopt : std::optional<std::uint64_t>(it->second)));

		auto ub = ref.upper_bound(probe);
		auto got = m.floor(probe);
		auto got_value = m.floor_value(probe);
		if (ub == ref.begin()) {
			CHECK(!got);
			CHECK(!got_value);
		} else {
			--ub;
			CHECK(got && got->first == ub->first && got->second == ub->second);
			CHECK(got_value == ub->second);
		}

		std::string lo = random_key(), hi = random_key();
		if (hi < lo) { std::swap(lo, hi); }
		if (rng() % 4 == 0) { hi.clear(); }
		reference want;
		for (auto r = ref.lower_bound(lo); r != ref.end() && (hi.empty() || r->first < hi); ++r) { want.insert(*r); }
		reference seen;
		m.range(lo, hi, [&](std::string_view k, std::uint64_t v) { seen.emplace(std::string(k), v); return true; });
		CHECK(seen == want);

		std::string prefix = random_key().substr(0, rng() % 3);
		reference with_prefix;
		for (const auto& [k, v] : ref) { if (k.starts_with(prefix)) { with_prefix.emplace(k, v); } }
		reference matched;
		std::string previous;
		bool ordered = true;
		m.intersect(fst::prefix_automaton{prefix}, [&](std::string_view k, std::uint64_t v) {
			ordered = ordered && (matched.empty() || std::string(k) > previous);
			previous = k;
			matched.emplace(std::string(k), v);
			return true;
		});
		CHECK(ordered);
		CHECK(matched == with_prefix);
	}

	// Early stop.
	std::size_t calls = 0;
	m.for_each([&](std::string_view, std::uint64_t) { return ++calls < 3; });
	CHECK(calls == std::min<std::size_t>(3, ref.size()));
}

int main(int argc, char** argv) {
	std::mt19937_64 rng(20261011);

	// Small alphabets force shared prefixes and suffixes; outputs vary from 0 to 2^63.
	for (int round = 0; round < 300; ++round) {
		std::string alphabet = round % 3 == 0 ? "ab" : round % 3 == 1 ? "abcdefgh" : "";
		if (alphabet.empty()) { for (int c = 0; c < 256; ++c) { alphabet.push_back(char(c)); } }
		reference ref;
		std::size_t n = rng() % 400;
		for (std::size_t i = 0; i < n; ++i) {
			std::string k;
			std::size_t len = rng() % 7;
			for (std::size_t j = 0; j < len; ++j) { k.push_back(alphabet[rng() % alphabet.size()]); }
			std::uint64_t v = round % 4 == 0 ? 0 : (rng() >> (rng() % 64));
			ref[k] = v;
		}
		check(ref, rng, alphabet);
	}

	// An empty FST.
	{
		std::string bytes = fst::builder{}.finish();
		fst::map m(bytes);
		CHECK(m.size() == 0);
		CHECK(!m.get(""));
		CHECK(!m.floor("zzz"));
	}

	// Out-of-order and duplicate keys are rejected.
	{
		fst::builder b;
		b.insert("b", 1);
		bool threw = false;
		try { b.insert("a", 2); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
		threw = false;
		try { b.insert("b", 2); } catch (const std::invalid_argument&) { threw = true; }
		CHECK(threw);
	}

	// A real vocabulary, if one is given or available.
	const char* words = argc > 1 ? argv[1] : "/usr/share/dict/words";
	if (std::ifstream in(words); in) {
		reference ref;
		std::string w;
		std::uint64_t ordinal = 0;
		while (std::getline(in, w)) { if (!w.empty()) { ref.emplace(w, 0); } }
		for (auto& [k, v] : ref) { v = ordinal++; }
		check(ref, rng, "abcdefghijklmnopqrstuvwxyzAB", 20);
	}

	std::printf("fst: %s\n", failures ? "FAILED" : "ok");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
