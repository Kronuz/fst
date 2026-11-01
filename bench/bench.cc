// FST against reference structures and well-known comparators on a vocabulary (one key per
// line): build rate, bytes per key, lookup time, and a check that every structure agrees.
//   fst_bench KEYS_FILE
// std::map and std::unordered_map are always in; absl::flat_hash_map and marisa-trie are in
// when CMake is configured with -DFST_BENCH_COMPARATORS=ON.
#include "fst.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef FST_BENCH_ABSEIL
#include "absl/container/flat_hash_map.h"
#endif
#ifdef FST_BENCH_MARISA
#include <marisa.h>
#endif

#if defined(__APPLE__)
#include <malloc/malloc.h>
static std::size_t block_size(void* p) { return malloc_size(p); }
#elif defined(__FreeBSD__)
#include <malloc_np.h>
static std::size_t block_size(void* p) { return malloc_usable_size(p); }
#else
#include <malloc.h>
static std::size_t block_size(void* p) { return malloc_usable_size(p); }
#endif

// Live heap bytes (the allocator's block sizes), so a structure's own memory is counted the
// same way for every node-based or table-based container.
static long long g_live = 0;
void* operator new(std::size_t n) {
	void* p = std::malloc(n ? n : 1);
	if (!p) { throw std::bad_alloc(); }
	g_live += (long long)block_size(p);
	return p;
}
void operator delete(void* p) noexcept {
	if (p) { g_live -= (long long)block_size(p); }
	std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

using clock_type = std::chrono::steady_clock;

static double seconds_since(clock_type::time_point t) { return std::chrono::duration<double>(clock_type::now() - t).count(); }

// Linear probing over a power-of-two table; keys live in one arena.
class hash_map {
public:
	explicit hash_map(const std::vector<std::string>& keys) {
		std::size_t cap = 1;
		while (cap < keys.size() * 2) { cap <<= 1; }
		slots_.assign(cap, slot{});
		mask_ = cap - 1;
		for (std::size_t i = 0; i < keys.size(); ++i) {
			std::uint32_t off = std::uint32_t(arena_.size());
			arena_ += keys[i];
			std::size_t h = hash(keys[i]);
			std::size_t at = h & mask_;
			while (slots_[at].len) { at = (at + 1) & mask_; }
			slots_[at] = slot{off, std::uint32_t(keys[i].size()), std::uint32_t(h >> 32), i};
		}
	}
	std::optional<std::uint64_t> get(std::string_view key) const noexcept {
		std::size_t h = hash(key);
		for (std::size_t at = h & mask_;; at = (at + 1) & mask_) {
			const slot& s = slots_[at];
			if (!s.len) { return std::nullopt; }
			if (s.tag == std::uint32_t(h >> 32) && s.len == key.size() && std::memcmp(arena_.data() + s.off, key.data(), key.size()) == 0) { return s.value; }
		}
	}
	std::size_t bytes() const noexcept { return slots_.size() * sizeof(slot) + arena_.size(); }

private:
	struct slot { std::uint32_t off = 0, len = 0, tag = 0; std::uint64_t value = 0; };
	static std::size_t hash(std::string_view k) noexcept {  // FNV-1a 64
		std::uint64_t h = 1469598103934665603ull;
		for (unsigned char c : k) { h = (h ^ c) * 1099511628211ull; }
		return std::size_t(h);
	}
	std::vector<slot> slots_;
	std::string arena_;
	std::size_t mask_ = 0;
};

// Front coding in blocks of 16: the first key whole, the rest as (shared, suffix).
class front_coded {
public:
	explicit front_coded(const std::vector<std::string>& keys) {
		for (std::size_t i = 0; i < keys.size(); ++i) {
			if (i % 16 == 0) {
				heads_.push_back(std::uint32_t(data_.size()));
				fst::detail::put_varint(data_, keys[i].size());
				data_ += keys[i];
			} else {
				const std::string& prev = keys[i - 1];
				std::size_t shared = 0;
				while (shared < prev.size() && shared < keys[i].size() && prev[shared] == keys[i][shared]) { ++shared; }
				fst::detail::put_varint(data_, shared);
				fst::detail::put_varint(data_, keys[i].size() - shared);
				data_.append(keys[i], shared);
			}
		}
		count_ = keys.size();
	}
	std::optional<std::uint64_t> get(std::string_view key) const {
		std::size_t lo = 0, hi = heads_.size();
		while (hi - lo > 1) {
			std::size_t mid = (lo + hi) / 2;
			if (head(mid) <= key) { lo = mid; } else { hi = mid; }
		}
		const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data_.data()) + heads_[lo];
		std::string cur;
		for (std::size_t i = lo * 16; i < std::min(count_, lo * 16 + 16); ++i) {
			if (i % 16 == 0) {
				std::size_t n = fst::detail::get_varint(p);
				cur.assign(reinterpret_cast<const char*>(p), n);
				p += n;
			} else {
				std::size_t shared = fst::detail::get_varint(p), n = fst::detail::get_varint(p);
				cur.resize(shared);
				cur.append(reinterpret_cast<const char*>(p), n);
				p += n;
			}
			if (cur == key) { return i; }
			if (cur > key) { break; }
		}
		return std::nullopt;
	}
	std::size_t bytes() const noexcept { return data_.size() + heads_.size() * 4; }

private:
	std::string_view head(std::size_t b) const {
		const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data_.data()) + heads_[b];
		std::size_t n = fst::detail::get_varint(p);
		return {reinterpret_cast<const char*>(p), n};
	}
	std::string data_;
	std::vector<std::uint32_t> heads_;
	std::size_t count_ = 0;
};

// Median nanoseconds per lookup over batches of 1024 shuffled present keys.
template <typename Lookup>
static double median_lookup_ns(const std::vector<std::string_view>& queries, Lookup&& lookup) {
	std::vector<double> batches;
	volatile std::uint64_t sink = 0;
	for (std::size_t start = 0; start + 1024 <= queries.size() && batches.size() < 2000; start += 1024) {
		auto t = clock_type::now();
		for (std::size_t i = start; i < start + 1024; ++i) { sink = sink + *lookup(queries[i]); }
		batches.push_back(seconds_since(t) * 1e9 / 1024);
	}
	std::nth_element(batches.begin(), batches.begin() + batches.size() / 2, batches.end());
	return batches[batches.size() / 2];
}

// Transparent hash and equality so unordered_map finds a string_view without a copy.
struct sv_hash {
	using is_transparent = void;
	std::size_t operator()(std::string_view k) const noexcept { return std::hash<std::string_view>()(k); }
};
struct sv_equal {
	using is_transparent = void;
	bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};
using std_map = std::map<std::string, std::uint64_t, std::less<>>;
using std_unordered_map = std::unordered_map<std::string, std::uint64_t, sv_hash, sv_equal>;

// Best of `repeats` wall times of one construction.
template <typename Make>
static double best_build_seconds(int repeats, Make&& make) {
	double best = 1e300;
	for (int i = 0; i < repeats; ++i) {
		auto t = clock_type::now();
		auto built = make();
		best = std::min(best, seconds_since(t));
		(void)built;
	}
	return best;
}

// Heap bytes the structure built by `make` keeps alive (the temporaries are gone by then).
template <typename Make>
static std::size_t live_heap_bytes(Make&& make) {
	long long before = g_live;
	auto built = make();
	long long after = g_live;
	(void)built;
	return std::size_t(after - before);
}

static void require_equal(const char* name, std::string_view key, std::optional<std::uint64_t> got, std::uint64_t want) {
	if (!got || *got != want) {
		std::fprintf(stderr, "MISMATCH: %s returned %s for \"%.*s\", expected ordinal %llu\n", name,
			got ? std::to_string(*got).c_str() : "nothing", int(key.size()), key.data(), (unsigned long long)want);
		std::abort();
	}
}

static void require_absent(const char* name, std::string_view key, bool found) {
	if (found) {
		std::fprintf(stderr, "MISMATCH: %s found the absent key \"%.*s\"\n", name, int(key.size()), key.data());
		std::abort();
	}
}

struct result {
	std::string name;
	std::size_t bytes = 0;
	double build_s = 0, lookup_ns = 1e300;
};

int main(int argc, char** argv) {
	const char* path = argc > 1 ? argv[1] : "/usr/share/dict/words";
	std::ifstream in(path);
	std::vector<std::string> keys;
	std::size_t key_bytes = 0;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty()) { keys.push_back(line); }
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	for (const auto& k : keys) { key_bytes += k.size(); }
	std::printf("%zu keys, %.2f bytes per key on average\n\n", keys.size(), double(key_bytes) / double(keys.size()));

	constexpr int build_repeats = 5;
	const auto make_fst = [&] {
		fst::builder b;
		for (std::size_t i = 0; i < keys.size(); ++i) { b.insert(keys[i], i); }
		return b.finish();
	};
	const auto make_std_map = [&] {
		auto m = std::make_unique<std_map>();
		for (std::size_t i = 0; i < keys.size(); ++i) { m->emplace_hint(m->end(), keys[i], i); }
		return m;
	};
	const auto make_std_unordered_map = [&] {
		auto m = std::make_unique<std_unordered_map>();
		m->reserve(keys.size());
		for (std::size_t i = 0; i < keys.size(); ++i) { m->emplace(keys[i], i); }
		return m;
	};

	std::vector<result> results;
	auto add = [&](const char* name, std::size_t bytes, double build_s) {
		results.push_back(result{name, bytes, build_s, 1e300});
		return results.size() - 1;
	};

	std::string bytes = make_fst();
	std::size_t r_fst = add("fst", bytes.size(), best_build_seconds(build_repeats, make_fst));
	fst::map m(bytes);

	std::size_t r_hash = add("hash map", 0, best_build_seconds(build_repeats, [&] { return hash_map(keys); }));
	hash_map h(keys);
	results[r_hash].bytes = h.bytes();
	std::size_t r_fc = add("front-coded", 0, best_build_seconds(build_repeats, [&] { return front_coded(keys); }));
	front_coded fc(keys);
	results[r_fc].bytes = fc.bytes();

	std::size_t r_map = add("std::map", live_heap_bytes(make_std_map), best_build_seconds(build_repeats, make_std_map));
	auto smap = make_std_map();
	std::size_t r_umap = add("std::unordered_map", live_heap_bytes(make_std_unordered_map), best_build_seconds(build_repeats, make_std_unordered_map));
	auto sumap = make_std_unordered_map();

#ifdef FST_BENCH_ABSEIL
	using absl_map = absl::flat_hash_map<std::string, std::uint64_t>;
	const auto make_absl = [&] {
		auto a = std::make_unique<absl_map>();
		a->reserve(keys.size());
		for (std::size_t i = 0; i < keys.size(); ++i) { a->emplace(keys[i], i); }
		return a;
	};
	std::size_t r_absl = add("absl::flat_hash_map", live_heap_bytes(make_absl), best_build_seconds(build_repeats, make_absl));
	auto amap = make_absl();
#endif
#ifdef FST_BENCH_MARISA
	const auto make_marisa = [&] {
		auto t = std::make_unique<marisa::Trie>();
		marisa::Keyset ks;
		for (const auto& k : keys) { ks.push_back(k.data(), k.size()); }
		t->build(ks);
		return t;
	};
	auto mtrie = make_marisa();
	std::size_t r_marisa = add("marisa-trie", mtrie->io_size(), best_build_seconds(build_repeats, make_marisa));
	marisa::Agent magent;
	const auto marisa_id = [&](std::string_view k) -> std::optional<std::uint64_t> {
		magent.set_query(k.data(), k.size());
		if (!mtrie->lookup(magent)) { return std::nullopt; }
		return magent.key().id();
	};
#endif

	// Every structure returns the key's ordinal for every key and nothing for an absent key.
	// marisa-trie numbers the keys itself, so its ids are checked to be a bijection onto
	// [0, n) that reverse_lookup maps back to the key.
	{
		std::size_t absent_checked = 0;
#ifdef FST_BENCH_MARISA
		std::vector<bool> seen(keys.size(), false);
#endif
		for (std::size_t i = 0; i < keys.size(); ++i) {
			const std::string& k = keys[i];
			require_equal("fst", k, m.get(k), i);
			require_equal("hash map", k, h.get(k), i);
			require_equal("front-coded", k, fc.get(k), i);
			auto it = smap->find(std::string_view(k));
			require_equal("std::map", k, it == smap->end() ? std::nullopt : std::optional<std::uint64_t>(it->second), i);
			auto uit = sumap->find(std::string_view(k));
			require_equal("std::unordered_map", k, uit == sumap->end() ? std::nullopt : std::optional<std::uint64_t>(uit->second), i);
#ifdef FST_BENCH_ABSEIL
			auto ait = amap->find(std::string_view(k));
			require_equal("absl::flat_hash_map", k, ait == amap->end() ? std::nullopt : std::optional<std::uint64_t>(ait->second), i);
#endif
#ifdef FST_BENCH_MARISA
			auto id = marisa_id(k);
			if (!id || *id >= keys.size() || seen[*id]) { std::fprintf(stderr, "MISMATCH: marisa-trie id for \"%s\" is missing, out of range or repeated\n", k.c_str()); std::abort(); }
			seen[*id] = true;
			magent.set_query(std::size_t(*id));
			mtrie->reverse_lookup(magent);
			if (std::string_view(magent.key().ptr(), magent.key().length()) != k) { std::fprintf(stderr, "MISMATCH: marisa-trie reverse_lookup of %llu is not \"%s\"\n", (unsigned long long)*id, k.c_str()); std::abort(); }
#endif
			if (i % 7 == 0) {
				const std::string absent = k + "\x01";
				require_absent("fst", absent, m.get(absent).has_value());
				require_absent("hash map", absent, h.get(absent).has_value());
				require_absent("front-coded", absent, fc.get(absent).has_value());
				require_absent("std::map", absent, smap->find(std::string_view(absent)) != smap->end());
				require_absent("std::unordered_map", absent, sumap->find(std::string_view(absent)) != sumap->end());
#ifdef FST_BENCH_ABSEIL
				require_absent("absl::flat_hash_map", absent, amap->find(std::string_view(absent)) != amap->end());
#endif
#ifdef FST_BENCH_MARISA
				require_absent("marisa-trie", absent, marisa_id(absent).has_value());
#endif
				++absent_checked;
			}
		}
		std::printf("verified: every structure returns the same ordinal for all %zu keys and finds none of %zu absent keys\n\n", keys.size(), absent_checked);
	}

	std::vector<std::string_view> queries;
	std::mt19937_64 rng(3);
	for (int rep = 0; rep < 4; ++rep) {
		for (const auto& k : keys) { queries.push_back(k); }
	}
	std::shuffle(queries.begin(), queries.end(), rng);
	if (queries.size() > 2000 * 1024) { queries.resize(2000 * 1024); }

	// Interleave the lookups three times; keep each one's best median.
	auto timed = [&](std::size_t r, auto&& lookup) { results[r].lookup_ns = std::min(results[r].lookup_ns, median_lookup_ns(queries, lookup)); };
	for (int round = 0; round < 3; ++round) {
		timed(r_fst, [&](std::string_view k) { return m.get(k); });
		timed(r_hash, [&](std::string_view k) { return h.get(k); });
		timed(r_fc, [&](std::string_view k) { return fc.get(k); });
		timed(r_map, [&](std::string_view k) { return std::optional<std::uint64_t>(smap->find(k)->second); });
		timed(r_umap, [&](std::string_view k) { return std::optional<std::uint64_t>(sumap->find(k)->second); });
#ifdef FST_BENCH_ABSEIL
		timed(r_absl, [&](std::string_view k) { return std::optional<std::uint64_t>(amap->find(k)->second); });
#endif
#ifdef FST_BENCH_MARISA
		timed(r_marisa, [&](std::string_view k) { return marisa_id(k); });
#endif
	}

	std::printf("%-20s %14s %12s %14s %14s\n", "structure", "bytes", "bytes/key", "build Mkeys/s", "lookup p50 ns");
	for (const auto& r : results) {
		std::printf("%-20s %14zu %12.2f %14.2f %14.1f\n", r.name.c_str(), r.bytes, double(r.bytes) / double(keys.size()), double(keys.size()) / r.build_s / 1e6, r.lookup_ns);
	}
	std::printf("\nfst / hash map: lookup %.2fx, bytes %.2f\n", results[r_fst].lookup_ns / results[r_hash].lookup_ns, double(results[r_fst].bytes) / double(results[r_hash].bytes));

	// Block-tree use: an FST over every 32nd key; floor() finds a key's block.
	fst::builder bb;
	for (std::size_t i = 0; i < keys.size(); i += 32) { bb.insert(keys[i], i / 32); }
	std::string index_bytes = bb.finish();
	fst::map index(index_bytes);
	double floor_ns = 1e300;
	for (int round = 0; round < 3; ++round) {
		floor_ns = std::min(floor_ns, median_lookup_ns(queries, [&](std::string_view k) {
			auto f = index.floor_value(k);
			return std::optional<std::uint64_t>(f.value_or(0));
		}));
	}
	std::printf("block index (1/32 of the keys, floor): %zu bytes, %.2f bytes per key, %.1f ns per floor (%.2fx the hash map lookup)\n",
		index.bytes(), double(index.bytes()) / double(keys.size()), floor_ns, floor_ns / results[r_hash].lookup_ns);
}
