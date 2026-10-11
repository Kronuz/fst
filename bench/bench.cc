// FST against an open-addressing hash map and a front-coded sorted array on a
// vocabulary (one key per line): build rate, bytes per key, and lookup time.
//   fst_bench KEYS_FILE
#include "fst.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

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

	auto t = clock_type::now();
	fst::builder b;
	for (std::size_t i = 0; i < keys.size(); ++i) { b.insert(keys[i], i); }
	std::string bytes = b.finish();
	double fst_build = seconds_since(t);
	fst::map m(bytes);

	t = clock_type::now();
	hash_map h(keys);
	double hash_build = seconds_since(t);
	t = clock_type::now();
	front_coded fc(keys);
	double fc_build = seconds_since(t);

	std::vector<std::string_view> queries;
	std::mt19937_64 rng(3);
	for (int rep = 0; rep < 4; ++rep) {
		for (const auto& k : keys) { queries.push_back(k); }
	}
	std::shuffle(queries.begin(), queries.end(), rng);
	if (queries.size() > 2000 * 1024) { queries.resize(2000 * 1024); }

	// Interleave the three lookups three times; keep each one's best median.
	double fst_ns = 1e300, hash_ns = 1e300, fc_ns = 1e300;
	for (int round = 0; round < 3; ++round) {
		fst_ns = std::min(fst_ns, median_lookup_ns(queries, [&](std::string_view k) { return m.get(k); }));
		hash_ns = std::min(hash_ns, median_lookup_ns(queries, [&](std::string_view k) { return h.get(k); }));
		fc_ns = std::min(fc_ns, median_lookup_ns(queries, [&](std::string_view k) { return fc.get(k); }));
	}

	std::printf("%-14s %14s %12s %14s %14s\n", "structure", "bytes", "bytes/key", "build Mkeys/s", "lookup p50 ns");
	auto row = [&](const char* name, std::size_t b, double build, double ns) {
		std::printf("%-14s %14zu %12.2f %14.2f %14.1f\n", name, b, double(b) / double(keys.size()), double(keys.size()) / build / 1e6, ns);
	};
	row("fst", m.bytes(), fst_build, fst_ns);
	row("hash map", h.bytes(), hash_build, hash_ns);
	row("front-coded", fc.bytes(), fc_build, fc_ns);
	std::printf("\nfst / hash map: lookup %.2fx, bytes %.2f\n", fst_ns / hash_ns, double(m.bytes()) / double(h.bytes()));

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
	std::printf("block index (1/32 of the keys, floor): %zu bytes, %.1f ns per floor (%.2fx the hash map lookup)\n",
		index.bytes(), floor_ns, floor_ns / hash_ns);
}
