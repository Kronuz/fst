// libFuzzer target: arbitrary bytes either fail validation or answer every query without
// leaving the buffer; FSTs built from input-derived keys validate and look up exactly.
#include "fst.hh"

#include <cstdlib>
#include <map>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	std::string_view bytes(reinterpret_cast<const char*>(data), size);
	try {
		fst::map m(bytes);
		std::size_t n = 0;
		for (std::size_t i = 0; i + 4 <= size && i < 64; i += 4) {
			std::string_view key = bytes.substr(i, 1 + data[i] % 6);
			(void)m.get(key);
			(void)m.floor(key);
			(void)m.floor_value(key);
		}
		m.range({}, {}, [&](std::string_view, std::uint64_t) { return ++n < 10000; });
		m.intersect(fst::prefix_automaton{bytes.substr(0, size % 3)}, [&](std::string_view, std::uint64_t) { return ++n < 20000; });
	} catch (const std::invalid_argument&) {
	}

	// Keys: the input split at 0x00 bytes; values: running positions.
	std::map<std::string, std::uint64_t> keys;
	std::size_t start = 0;
	for (std::size_t i = 0; i <= size; ++i) {
		if (i == size || data[i] == 0) {
			keys.emplace(std::string(bytes.substr(start, i - start)), std::uint64_t(start) * 0x9e3779b97f4a7c15ull);
			start = i + 1;
		}
	}
	fst::builder b;
	for (const auto& [k, v] : keys) { b.insert(k, v); }
	std::string built = b.finish();
	fst::map m(built);  // must validate
	for (const auto& [k, v] : keys) {
		if (m.get(k) != v || m.floor_value(k) != v) { std::abort(); }
	}
	return 0;
}
