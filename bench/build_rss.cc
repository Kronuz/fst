// Peak memory of one fst build, as the operating system sees it: the peak resident set of a
// process that builds an FST from N sorted random 12-letter keys, with fst::builder and with the
// reference builder in test/fst_old.hh (one std::string signature per node). The peak includes the
// sorted keys, which the process holds while it builds. Each measurement runs in its own process,
// because a peak never falls.
//   fst_build_rss                       the default sweep, both builders, with the ratio
//   fst_build_rss N [N...]              the same for the given key counts
//   fst_build_rss new|reference N       one measurement, one line: builder bytes, peak RSS, RSS before
//                                       the build and a hash of the FST bytes
// test/memory.cc counts the heap through operator new and checks the builder never needs more
// than the reference; this bench reports the resident set the README and the notes quote.
#include "fst.hh"
#include "fst_old.hh"

#include <sys/resource.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// ru_maxrss is in bytes on macOS and in KiB elsewhere.
static double max_rss_bytes() {
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
	return double(usage.ru_maxrss);
#else
	return double(usage.ru_maxrss) * 1024.0;
#endif
}

static std::uint64_t fnv1a(const std::string& bytes) {
	std::uint64_t h = 1469598103934665603ull;
	for (unsigned char c : bytes) { h = (h ^ c) * 1099511628211ull; }
	return h;
}

template <typename Builder>
static std::string build(const std::vector<std::string>& keys) {
	Builder b;
	for (std::size_t i = 0; i < keys.size(); ++i) { b.insert(keys[i], i); }
	return b.finish();
}

// One measurement, in this process: prints "fst_bytes peak_rss before_rss hash".
static int measure(bool use_builder, std::size_t n) {
	std::mt19937_64 rng(1);
	std::vector<std::string> keys;
	keys.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		std::string k;
		for (int j = 0; j < 12; ++j) { k.push_back(char('a' + rng() % 26)); }
		keys.push_back(std::move(k));
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	const double before = max_rss_bytes();
	const std::string bytes = use_builder ? build<fst::builder>(keys) : build<fst_old::builder>(keys);
	std::printf("%zu %.0f %.0f %" PRIu64 "\n", bytes.size(), max_rss_bytes(), before, fnv1a(bytes));
	return 0;
}

struct result {
	std::size_t fst_bytes = 0;
	double peak = 0, before = 0;
	std::uint64_t hash = 0;
};

// Runs one measurement in a child process (this program again) and reads its line.
static bool run_child(const char* self, const char* variant, std::size_t n, result& r) {
	std::string command = "'";
	for (const char* c = self; *c; ++c) {
		if (*c == '\'') { command += "'\\''"; } else { command += *c; }
	}
	command += "' " + std::string(variant) + " " + std::to_string(n);
	FILE* pipe = popen(command.c_str(), "r");
	if (!pipe) { return false; }
	const int fields = std::fscanf(pipe, "%zu %lf %lf %" SCNu64, &r.fst_bytes, &r.peak, &r.before, &r.hash);
	return pclose(pipe) == 0 && fields == 4;
}

int main(int argc, char** argv) {
	if (argc == 3 && (!std::strcmp(argv[1], "new") || !std::strcmp(argv[1], "reference"))) {
		return measure(!std::strcmp(argv[1], "new"), std::strtoull(argv[2], nullptr, 10));
	}
	std::vector<std::size_t> sizes;
	for (int i = 1; i < argc; ++i) { sizes.push_back(std::strtoull(argv[i], nullptr, 10)); }
	if (sizes.empty()) { sizes = {100000, 300000, 1000000, 2000000, 3000000}; }
	const double mib = 1024.0 * 1024.0;
	std::printf("peak RSS of a process that builds one FST from N random 12-letter keys, the sorted keys included\n");
	std::printf("%10s %12s %12s %14s %14s %9s\n", "keys", "fst bytes", "keys MiB", "reference MiB", "builder MiB", "ratio");
	int status = 0;
	for (std::size_t n : sizes) {
		result reference, builder;
		if (!run_child(argv[0], "reference", n, reference) || !run_child(argv[0], "new", n, builder)) {
			std::fprintf(stderr, "measurement failed for %zu keys\n", n);
			return 1;
		}
		if (reference.hash != builder.hash || reference.fst_bytes != builder.fst_bytes) {
			std::fprintf(stderr, "FAIL: %zu keys: the builders produced different bytes\n", n);
			status = 1;
		}
		std::printf("%10zu %12zu %12.1f %14.1f %14.1f %8.2fx\n", n, builder.fst_bytes, builder.before / mib, reference.peak / mib, builder.peak / mib,
		            builder.peak / reference.peak);
		std::fflush(stdout);
	}
	return status;
}
