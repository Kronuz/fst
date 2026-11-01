// Timings of fst.hh against the reference test/fst_old.hh, each with an A/A row (the reference
// against itself) for the noise floor.
//   fst_cmp_bench [WORDS_FILE [EDGE_ROW_FILTER]]
// The first rows are the builder and full iteration over a word list (one key per line). The
// edge rows follow, on generated keys: builds from 1 to 300,000 keys, decimal, random and
// 200-byte keys, `for_each`, short and long `range` calls, prefix `intersect` and `get`.
// EDGE_ROW_FILTER keeps the groups of edge rows whose name contains it: build, random (`for_each`,
// `range`, `intersect` and `get` on 200,000 random keys), decimal or long.
#include "fst.hh"
#include "fst_old.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

template <typename F>
static double best_ms(F&& f, int reps = 15) {
	double best = 1e100;
	for (int r = 0; r < reps; ++r) {
		auto t0 = std::chrono::steady_clock::now();
		f();
		best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}
	return best;
}

template <typename NS>
struct impl;
template <>
struct impl<fst::map> {
	static std::string build(const std::vector<std::string>& k) {
		fst::builder b;
		for (std::size_t i = 0; i < k.size(); ++i) { b.insert(k[i], i); }
		return b.finish();
	}
};
template <>
struct impl<fst_old::map> {
	static std::string build(const std::vector<std::string>& k) {
		fst_old::builder b;
		for (std::size_t i = 0; i < k.size(); ++i) { b.insert(k[i], i); }
		return b.finish();
	}
};

template <typename M>
static double iterate_ms(const std::string& bytes) {
	M m(bytes);
	return best_ms([&] {
		std::uint64_t s = 0;
		m.for_each([&](std::string_view k, std::uint64_t v) { s += k.size() + v; return true; });
		volatile std::uint64_t sink = s;
		(void)sink;
	});
}

static volatile std::uint64_t sink;

// Time of one call in ns, averaged over at least 20 ms of calls.
template <typename F>
static double ns_per_call(F&& f) {
	std::size_t calls = 0;
	const auto start = std::chrono::steady_clock::now();
	double s = 0;
	do {
		f();
		++calls;
		s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	} while (s < 0.02);
	return s * 1e9 / double(calls);
}

// Interleaved trials: reference, reference again (A/A), builder. Prints the best of each and the
// median and range of the reference/builder and reference/reference ratios. A call that handles
// `per` keys is reported in ns per key; with `per` 0 it is reported in ns per call.
template <typename Old, typename New>
static void row(const char* name, Old&& old_call, New&& new_call, double per = 0, int trials = 11) {
	double best_old = 1e300, best_new = 1e300;
	std::vector<double> ratio, noise;
	for (int t = 0; t < trials; ++t) {
		const double x = ns_per_call(old_call), y = ns_per_call(old_call), z = ns_per_call(new_call);
		best_old = std::min(best_old, x);
		best_new = std::min(best_new, z);
		ratio.push_back(x / z);
		noise.push_back(x / y);
	}
	std::sort(ratio.begin(), ratio.end());
	std::sort(noise.begin(), noise.end());
	const std::size_t mid = ratio.size() / 2, lo = ratio.size() / 10, hi = ratio.size() - 1 - lo;
	std::printf("%-38s old %10.1f  new %10.1f ns/%s  old/new %5.2fx [%.2f..%.2f]  A/A %5.2fx [%.2f..%.2f]\n", name, best_old / (per ? per : 1), best_new / (per ? per : 1),
	            per ? "key " : "call", ratio[mid], ratio[lo], ratio[hi], noise[mid], noise[lo], noise[hi]);
	std::fflush(stdout);
}

using key_set = std::vector<std::pair<std::string, std::uint64_t>>;

template <typename Builder>
static std::string build_set(const key_set& keys) {
	Builder b;
	for (const auto& [key, value] : keys) { b.insert(key, value); }
	return b.finish();
}

// Distinct keys of minlen to maxlen letters from the first `alphabet` lowercase letters, sorted.
static key_set random_keys(std::mt19937_64& rng, std::size_t n, int alphabet, std::size_t maxlen, std::size_t minlen = 1) {
	std::map<std::string, std::uint64_t> sorted;
	while (sorted.size() < n) {
		std::string s;
		for (std::size_t length = minlen + rng() % (maxlen - minlen + 1); length; --length) { s.push_back(char('a' + rng() % alphabet)); }
		sorted[s] = rng() % 100000;
	}
	return key_set(sorted.begin(), sorted.end());
}

static void iterate_row(const char* name, const key_set& keys) {
	const std::string bytes = build_set<fst::builder>(keys);
	fst::map now(bytes);
	fst_old::map reference(bytes);
	row(name,
	    [&] { std::uint64_t t = 0; reference.for_each([&](std::string_view s, std::uint64_t v) { t += v + s.size(); return true; }); sink = sink + t; },
	    [&] { std::uint64_t t = 0; now.for_each([&](std::string_view s, std::uint64_t v) { t += v + s.size(); return true; }); sink = sink + t; },
	    double(keys.size()));
}

static void build_row(const char* name, const key_set& keys, int trials = 11) {
	row(name, [&] { sink = sink + build_set<fst_old::builder>(keys).size(); }, [&] { sink = sink + build_set<fst::builder>(keys).size(); }, double(keys.size()), trials);
}

// Rows on generated keys, in shapes the word list does not have: builds small enough that the
// registry's first table dominates, long chains, long keys and wide fan-out, and the short
// `range` calls of a term dictionary's block lookups.
static void edge_rows(const char* filter) {
	auto on = [&](const char* name) { return !*filter || std::string(name).find(filter) != std::string::npos; };
	std::mt19937_64 rng(5);
	std::printf("\nedge rows (old is the reference, new is fst.hh; ratios are old/new)\n");
	if (on("build")) {
		for (std::size_t n : {1, 4, 16, 64, 256, 1024, 4096}) {
			char name[64];
			std::snprintf(name, sizeof name, "build %zu random keys", n);
			build_row(name, random_keys(rng, n, 26, 10));
		}
		build_row("build 300k random 12-letter keys", random_keys(rng, 300000, 26, 12, 12), 5);
	}
	if (on("random")) {
		const key_set keys = random_keys(rng, 200000, 26, 12);
		iterate_row("for_each 200k random keys", keys);
		const std::string bytes = build_set<fst::builder>(keys);
		fst::map now(bytes);
		fst_old::map reference(bytes);
		std::vector<std::string> prefixes, queries;
		for (int i = 0; i < 1024; ++i) {
			prefixes.push_back(keys[rng() % keys.size()].first.substr(0, 3));
			queries.push_back(keys[rng() % keys.size()].first);
		}
		for (std::size_t limit : {1, 10, 100}) {
			char name[64];
			std::snprintf(name, sizeof name, "range from a prefix, first %zu keys", limit);
			std::size_t i = 0;
			row(name,
			    [&] { std::size_t c = 0; reference.range(prefixes[i++ & 1023], {}, [&](std::string_view s, std::uint64_t v) { sink = sink + v + s.size(); return ++c < limit; }); },
			    [&] { std::size_t c = 0; now.range(prefixes[i++ & 1023], {}, [&](std::string_view s, std::uint64_t v) { sink = sink + v + s.size(); return ++c < limit; }); });
		}
		std::size_t i = 0;
		row("intersect, 3-byte prefix, all keys",
		    [&] { reference.intersect(fst_old::prefix_automaton{prefixes[i++ & 1023]}, [&](std::string_view s, std::uint64_t v) { sink = sink + v + s.size(); return true; }); },
		    [&] { now.intersect(fst::prefix_automaton{prefixes[i++ & 1023]}, [&](std::string_view s, std::uint64_t v) { sink = sink + v + s.size(); return true; }); });
		row("get, 1024 keys",
		    [&] { for (const auto& q : queries) { sink = sink + *reference.get(q); } }, [&] { for (const auto& q : queries) { sink = sink + *now.get(q); } }, 1024);
	}
	if (on("decimal")) {
		std::map<std::string, std::uint64_t> sorted;  // long chains and deep suffix sharing
		for (int i = 0; i < 300000; ++i) { sorted[std::to_string(i)] = std::uint64_t(i); }
		const key_set keys(sorted.begin(), sorted.end());
		build_row("build 300k decimal keys", keys, 5);
		iterate_row("for_each 300k decimal keys", keys);
	}
	if (on("long")) {
		std::map<std::string, std::uint64_t> sorted;  // 200 bytes over two letters: key buffer growth and deep stacks
		while (sorted.size() < 20000) {
			std::string s;
			for (int j = 0; j < 200; ++j) { s.push_back(char('a' + rng() % 2)); }
			sorted[s] = rng() % 1000;
		}
		const key_set keys(sorted.begin(), sorted.end());
		build_row("build 20k keys of 200 bytes", keys, 5);
		iterate_row("for_each 20k keys of 200 bytes", keys);
	}
}

int main(int argc, char** argv) {
	std::ifstream in(argc > 1 ? argv[1] : "/usr/share/dict/words");
	std::vector<std::string> k;
	for (std::string l; std::getline(in, l);) { if (!l.empty()) { k.push_back(l); } }
	std::sort(k.begin(), k.end());
	k.erase(std::unique(k.begin(), k.end()), k.end());
	if (k.empty()) {
		std::printf("no word list: skipping the word-list rows\n");
		edge_rows(argc > 2 ? argv[2] : "");
		return 0;
	}
	std::string a = impl<fst::map>::build(k), b = impl<fst_old::map>::build(k);
	std::printf("keys %zu bytes new %zu old %zu identical=%d\n", k.size(), a.size(), b.size(), int(a == b));
	if (a != b) { return 1; }
	auto per = [&](double ms) { return ms * 1e6 / double(k.size()); };
	double o1 = best_ms([&] { impl<fst_old::map>::build(k); }), o2 = best_ms([&] { impl<fst_old::map>::build(k); });
	double n1 = best_ms([&] { impl<fst::map>::build(k); });
	std::printf("build  old %.1f ns/key  old(A/A) %.1f  new %.1f   speedup %.2fx (A/A %.2f)\n", per(o1), per(o2), per(n1), o1 / n1, o1 / o2);
	double io1 = iterate_ms<fst_old::map>(a), io2 = iterate_ms<fst_old::map>(a), in1 = iterate_ms<fst::map>(a);
	std::printf("for_each old %.1f ns/key  old(A/A) %.1f  new %.1f   speedup %.2fx (A/A %.2f)\n", per(io1), per(io2), per(in1), io1 / in1, io1 / io2);
	edge_rows(argc > 2 ? argv[2] : "");
}
