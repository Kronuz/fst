// Builder and full-iteration timings, new fst.hh against the frozen test/fst_old.hh, with an A/A
// row (old against old) for the noise floor. Usage: fst_cmp_bench [words-file]
#include "fst.hh"
#include "fst_old.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

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

int main(int argc, char** argv) {
	std::ifstream in(argc > 1 ? argv[1] : "/usr/share/dict/words");
	std::vector<std::string> k;
	for (std::string l; std::getline(in, l);) { if (!l.empty()) { k.push_back(l); } }
	std::sort(k.begin(), k.end());
	k.erase(std::unique(k.begin(), k.end()), k.end());
	std::string a = impl<fst::map>::build(k), b = impl<fst_old::map>::build(k);
	std::printf("keys %zu bytes new %zu old %zu identical=%d\n", k.size(), a.size(), b.size(), int(a == b));
	if (a != b) { return 1; }
	auto per = [&](double ms) { return ms * 1e6 / double(k.size()); };
	double o1 = best_ms([&] { impl<fst_old::map>::build(k); }), o2 = best_ms([&] { impl<fst_old::map>::build(k); });
	double n1 = best_ms([&] { impl<fst::map>::build(k); });
	std::printf("build  old %.1f ns/key  old(A/A) %.1f  new %.1f   speedup %.2fx (A/A %.2f)\n", per(o1), per(o2), per(n1), o1 / n1, o1 / o2);
	double io1 = iterate_ms<fst_old::map>(a), io2 = iterate_ms<fst_old::map>(a), in1 = iterate_ms<fst::map>(a);
	std::printf("for_each old %.1f ns/key  old(A/A) %.1f  new %.1f   speedup %.2fx (A/A %.2f)\n", per(io1), per(io2), per(in1), io1 / in1, io1 / io2);
}
