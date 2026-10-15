// fst: get, floor, range, and automaton intersection against std::map on random and
// dictionary key sets, including the empty key, shared suffixes, and wide nodes.
#include "fst.hh"

#include <cstdio>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <limits>
#include <thread>

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

	// Malformed bytes are rejected at open; anything accepted is safe to query.
	{
		reference ref;
		for (int i = 0; i < 500; ++i) { ref["k" + std::to_string(i * 7919 % 100003)] = std::uint64_t(i); }
		std::string good = build(ref);
		auto rejects = [](const std::string& bytes) {
			try { fst::map m(bytes); } catch (const std::invalid_argument&) { return true; }
			return false;
		};
		CHECK(!rejects(good));
		std::string bad = good;
		for (int i = 0; i < 8; ++i) { bad[bad.size() - 16 + std::size_t(i)] = char(0xff); }  // root far outside the bytes
		CHECK(rejects(bad));
		for (std::size_t cut = 0; cut < good.size(); cut += 1 + good.size() / 300) { CHECK(rejects(good.substr(0, cut))); }
		// A chain node whose delta is 0 (a self-loop): build a one-key FST and zero its delta.
		{
			fst::builder b;
			b.insert("a", 0);
			std::string one = b.finish();
			std::string loop = one;
			std::size_t root = std::size_t(fst::detail::get_le(reinterpret_cast<const std::uint8_t*>(loop.data()) + loop.size() - 16, 8));
			loop[root + 2] = 0;  // [header][label][delta]
			CHECK(rejects(loop));
		}
		std::mt19937_64 g(9);
		for (int round = 0; round < 20000; ++round) {
			std::string mutated = good;
			mutated[g() % mutated.size()] = char(g());
			try {
				fst::map m(mutated);
				for (int q = 0; q < 20; ++q) {
					std::string key = "k" + std::to_string(g() % 100003);
					(void)m.get(key);
					(void)m.floor(key);
					(void)m.floor_value(key);
				}
				std::size_t n = 0;
				m.range("k1", "k5", [&](std::string_view, std::uint64_t) { return ++n < 1000; });
				m.intersect(fst::prefix_automaton{"k2"}, [&](std::string_view, std::uint64_t) { return ++n < 2000; });
			} catch (const std::invalid_argument&) {
			}
		}
	}

	// Deep keys (1 MiB, past every engine's term limit) walk in constant native stack
	// (macOS threads have 512 KiB).
	{
		constexpr std::size_t deep = std::size_t(1) << 20;
		fst::builder b;
		std::string a(deep, 'a'), z(deep, 'z');
		b.insert(a, 1);
		b.insert(z, 2);
		std::string bytes = b.finish();
		std::size_t seen = 0, matched = 0, floored = 0;
		std::thread worker([&] {
			fst::map m(bytes);
			m.for_each([&](std::string_view k, std::uint64_t) { seen += k.size() == deep; return true; });
			m.intersect(fst::prefix_automaton{"zz"}, [&](std::string_view, std::uint64_t v) { matched += v == 2; return true; });
			auto f = m.floor(z + "a");
			floored = f && f->first == z;
		});
		worker.join();
		CHECK(seen == 2);
		CHECK(matched == 1);
		CHECK(floored == 1);
		static_assert(fst::max_key_length == std::numeric_limits<std::uint32_t>::max());
	}

	// Golden fixture: the FST1 format must not drift.
	{
		fst::builder b;
		const char* words[] = {"", "a", "ab", "abc", "abd", "b", "ba", "bab", "cab", "zzz"};
		std::uint64_t v = 0;
		for (const char* w : words) { b.insert(w, v); v = v * 31 + 7; }
		std::string bytes = b.finish();
		const char* path = FST_FIXTURES "/small.fst";
		if (std::getenv("FST_WRITE_FIXTURES")) {
			std::FILE* f = std::fopen(path, "wb");
			std::fwrite(bytes.data(), 1, bytes.size(), f);
			std::fclose(f);
		}
		std::string golden;
		if (std::FILE* f = std::fopen(path, "rb")) {
			for (int c; (c = std::fgetc(f)) != EOF;) { golden.push_back(char(c)); }
			std::fclose(f);
		}
		CHECK(golden == bytes);
		fst::map m(golden);
		v = 0;
		for (const char* w : words) { CHECK(m.get(w) == v); v = v * 31 + 7; }
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
