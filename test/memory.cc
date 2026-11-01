// Peak heap use of fst::builder against the reference builder in test/fst_old.hh (one
// std::string signature per node in an unordered_map): over a sweep of key counts, because the
// registry table doubles and so its peak saws with the key count, the new builder must never
// need more heap than the reference. Heap use is counted by replacing operator new and delete,
// which makes the numbers exact and independent of the allocator and the host.
#include "fst.hh"
#include "fst_old.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <vector>

// ThreadSanitizer's runtime defines operator new itself; the test is skipped there.
#if defined(__SANITIZE_THREAD__)
#define FST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FST_TSAN 1
#endif
#endif

static std::size_t live_bytes = 0, peak_bytes = 0;
#ifndef FST_TSAN
static constexpr std::size_t header = 16;  // keeps the block 16-byte aligned and records its size

void* operator new(std::size_t n) {
	void* p = std::malloc(n + header);
	if (!p) { throw std::bad_alloc(); }
	*static_cast<std::size_t*>(p) = n;
	live_bytes += n;
	peak_bytes = std::max(peak_bytes, live_bytes);
	return static_cast<char*>(p) + header;
}
void operator delete(void* p) noexcept {
	if (!p) { return; }
	void* block = static_cast<char*>(p) - header;
	live_bytes -= *static_cast<std::size_t*>(block);
	std::free(block);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
#endif

static int failures = 0;

// Peak heap above the starting level while building and finishing one FST, the result kept.
template <typename Builder>
static std::size_t build_peak(const std::vector<std::string>& keys) {
	const std::size_t base = live_bytes;
	peak_bytes = live_bytes;
	std::string bytes;
	{
		Builder b;
		for (std::size_t i = 0; i < keys.size(); ++i) { b.insert(keys[i], i); }
		bytes = b.finish();
	}
	return peak_bytes - base;
}

int main() {
#ifdef FST_TSAN
	std::puts("fst builder memory tests skipped under ThreadSanitizer");
	return 0;
#endif
	std::mt19937_64 rng(1);
	std::vector<std::string> keys;
	for (std::size_t i = 0; i < 2000000; ++i) {
		std::string k;
		for (int j = 0; j < 12; ++j) { k.push_back(char('a' + rng() % 26)); }
		keys.push_back(std::move(k));
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	std::shuffle(keys.begin(), keys.end(), rng);  // so that every prefix of the sweep is a random subset
	// Sizes around the points where the registry table doubles (about every 2x) and just after.
	const std::size_t sizes[] = {1000, 5000, 20000, 60000, 100000, 150000, 200000, 250000, 300000, 400000, 600000, 800000, 1000000, 1500000, 1999000};
	for (std::size_t n : sizes) {
		std::vector<std::string> sub(keys.begin(), keys.begin() + std::min(n, keys.size()));
		std::sort(sub.begin(), sub.end());
		const std::size_t old_peak = build_peak<fst_old::builder>(sub), new_peak = build_peak<fst::builder>(sub);
		std::printf("%8zu keys: reference %9zu B, builder %9zu B (%.2fx)\n", sub.size(), old_peak, new_peak, double(new_peak) / double(old_peak));
		if (new_peak > old_peak) {
			std::fprintf(stderr, "FAIL: %zu keys: builder peak %zu B exceeds the reference's %zu B\n", sub.size(), new_peak, old_peak);
			++failures;
		}
	}
	if (failures) { return 1; }
	std::puts("fst builder memory tests passed");
}
