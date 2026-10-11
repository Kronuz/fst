/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Minimal acyclic finite state transducers: an ordered map from byte strings to
// uint64 values, built from sorted keys and read in place from bytes.
//
// Outputs are additive: a key's value is the sum of the outputs on its path plus the
// final node's output. The builder minimizes incrementally (Daciuk et al., 2000),
// sharing equal suffixes, and pushes outputs toward the root.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__SSE2__) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace fst {

// ---------------------------------------------------------------------------------------------
// Format
//
// [magic "FST1"] [nodes] [root address: 8 bytes LE] [key count: 8 bytes LE]
//
// Nodes are written children first, so every target address is below its parent's;
// transitions store (parent address - target address).
//
// Header byte: bit 0 final; bit 1 final output follows; bit 2 chain node.
//   Chain (one transition): bit 3 transition output follows.
//     [header] [final output varint?] [label] [output varint?] [delta varint]
//   General: bits 3-7 hold n when n < 31, else 31 and a varint (n - 31) follows.
//     [header] [final output varint?] [n?] [labels: n ascending bytes]
//     [widths: out | target << 4] [outputs: n × out bytes LE] [deltas: n × target bytes LE]
//     A node with n = 0 (a leaf) has no labels or widths byte.
// The 16-byte trailer lets readers load 8 or 16 bytes past any field.

namespace detail {

inline void put_varint(std::string& out, std::uint64_t v) {
	while (v >= 0x80) {
		out.push_back(char(v | 0x80));
		v >>= 7;
	}
	out.push_back(char(v));
}

inline std::uint64_t get_varint(const std::uint8_t*& p) noexcept {
	std::uint64_t v = 0;
	for (unsigned shift = 0;; shift += 7) {
		std::uint8_t b = *p++;
		v |= std::uint64_t(b & 0x7f) << shift;
		if (!(b & 0x80)) { return v; }
	}
}

inline unsigned bytes_for(std::uint64_t v) noexcept {
	unsigned n = 0;
	for (; v; v >>= 8) { ++n; }
	return n;
}

// Reads an n-byte little-endian field (n <= 8) with one 8-byte load; the trailer
// guarantees the over-read stays in bounds.
inline std::uint64_t get_le(const std::uint8_t* p, unsigned n) noexcept {
	std::uint64_t v;
	std::memcpy(&v, p, 8);  // little-endian hosts
	return n == 8 ? v : v & ((std::uint64_t(1) << (8 * n)) - 1);
}

// Index of label among n ascending labels, or n.
inline std::size_t find_label(const std::uint8_t* labels, std::size_t n, std::uint8_t label) noexcept {
#if defined(__SSE2__) || defined(__x86_64__)
	const __m128i needle = _mm_set1_epi8(char(label));
	for (std::size_t i = 0; i < n; i += 16) {
		unsigned m = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(labels + i)), needle)));
		if (m) {
			std::size_t at = i + unsigned(__builtin_ctz(m));
			return at < n ? at : n;
		}
	}
	return n;
#else
	const std::uint8_t* it = std::lower_bound(labels, labels + n, label);
	return it != labels + n && *it == label ? std::size_t(it - labels) : n;
#endif
}

inline void put_le(std::string& out, std::uint64_t v, unsigned n) {
	for (unsigned i = 0; i < n; ++i) { out.push_back(char(v >> (8 * i))); }
}

inline constexpr std::uint8_t final_bit = 1, final_output_bit = 2, chain_bit = 4, chain_output_bit = 8;

}  // namespace detail

struct transition {
	std::uint8_t label;
	std::uint64_t output;
	std::uint64_t target;  // node address
};

// ---------------------------------------------------------------------------------------------
// Reader

// A read-only FST over bytes owned elsewhere (a file mapping, a string).
class map {
public:
	map() = default;
	explicit map(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
		if (bytes.size() < 20 || std::memcmp(bytes.data(), "FST1", 4) != 0) { throw std::invalid_argument("fst: not an FST"); }
		root_ = detail::get_le(bytes.data() + bytes.size() - 16, 8);
		size_ = detail::get_le(bytes.data() + bytes.size() - 8, 8);
	}
	explicit map(std::string_view bytes)
		: map(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())) {}

	std::uint64_t size() const noexcept { return size_; }
	std::size_t bytes() const noexcept { return bytes_.size(); }

	// A node decoded enough to search its transitions.
	struct node {
		std::uint64_t address = 0;
		bool is_final = false;
		std::uint64_t final_output = 0;
		std::size_t count = 0;
		transition only{};                    // chain nodes
		const std::uint8_t* labels = nullptr;  // general nodes
		const std::uint8_t* outputs = nullptr;
		const std::uint8_t* deltas = nullptr;
		unsigned out_bytes = 0, target_bytes = 0;

		transition at(std::size_t i) const noexcept {
			if (!labels) { return only; }
			return {labels[i], detail::get_le(outputs + i * out_bytes, out_bytes), address - detail::get_le(deltas + i * target_bytes, target_bytes)};
		}

		// Index of the transition with this label, or count.
		std::size_t find(std::uint8_t label) const noexcept {
			if (!labels) { return count && only.label == label ? 0 : count; }
			return detail::find_label(labels, count, label);
		}

		// Index of the largest label below this one, or count.
		std::size_t below(std::uint8_t label) const noexcept {
			if (!labels) { return count && only.label < label ? 0 : count; }
			const std::uint8_t* it = std::lower_bound(labels, labels + count, label);
			return it == labels ? count : std::size_t(it - labels) - 1;
		}
	};

	node load(std::uint64_t address) const noexcept {
		node n;
		n.address = address;
		const std::uint8_t* p = bytes_.data() + address;
		const std::uint8_t h = *p++;
		n.is_final = h & detail::final_bit;
		if (h & detail::final_output_bit) { n.final_output = detail::get_varint(p); }
		if (h & detail::chain_bit) {
			n.count = 1;
			n.only.label = *p++;
			n.only.output = (h & detail::chain_output_bit) ? detail::get_varint(p) : 0;
			n.only.target = address - detail::get_varint(p);
		} else {
			std::size_t count = h >> 3;
			if (count == 31) { count += std::size_t(detail::get_varint(p)); }
			n.count = count;
			if (count) {
				n.labels = p;
				p += count;
				const std::uint8_t widths = *p++;
				n.out_bytes = widths & 0x0f;
				n.target_bytes = widths >> 4;
				n.outputs = p;
				p += count * n.out_bytes;
				n.deltas = p;
			}
		}
		return n;
	}

	node root() const noexcept { return load(root_); }

	// The value of key, if present.
	std::optional<std::uint64_t> get(std::string_view key) const noexcept {
		const std::uint8_t* base = bytes_.data();
		std::uint64_t out = 0, address = root_;
		for (unsigned char c : key) {
			const std::uint8_t* p = base + address;
			const std::uint8_t h = *p++;
			if (h & detail::final_output_bit) { detail::get_varint(p); }
			if (h & detail::chain_bit) {
				if (*p++ != c) { return std::nullopt; }
				if (h & detail::chain_output_bit) { out += detail::get_varint(p); }
				address -= detail::get_varint(p);
				continue;
			}
			std::size_t count = h >> 3;
			if (count == 31) { count += std::size_t(detail::get_varint(p)); }
			std::size_t i = detail::find_label(p, count, c);
			if (i == count) { return std::nullopt; }
			p += count;
			const unsigned ob = *p & 0x0f, tb = *p >> 4;
			++p;
			out += detail::get_le(p + i * ob, ob);
			address -= detail::get_le(p + count * ob + i * tb, tb);
		}
		const std::uint8_t* p = base + address;
		const std::uint8_t h = *p++;
		if (!(h & detail::final_bit)) { return std::nullopt; }
		return (h & detail::final_output_bit) ? out + detail::get_varint(p) : out;
	}

	bool contains(std::string_view key) const noexcept { return get(key).has_value(); }

	// The largest key <= key, with its value.
	std::optional<std::pair<std::string, std::uint64_t>> floor(std::string_view key) const {
		struct frame { node n; std::uint64_t out; };
		std::vector<frame> path;  // path[d] is the node at depth d along key
		node n = load(root_);
		std::uint64_t out = 0;
		std::size_t depth = 0;
		for (; depth < key.size(); ++depth) {
			path.push_back({n, out});
			std::size_t i = n.find(std::uint8_t(key[depth]));
			if (i == n.count) { break; }
			transition t = n.at(i);
			out += t.output;
			n = load(t.target);
		}
		// Exact match; otherwise keys below the node reached at depth key.size() are longer
		// than key (greater), and a miss at depth < key.size() resumes from path.back().
		if (depth == key.size() && n.is_final) { return std::pair{std::string(key), out + n.final_output}; }
		// Walk back: at depth d, take the largest label below key[d] and descend to the
		// maximum; else, if the node at depth d is final, its prefix is the answer.
		while (!path.empty()) {
			std::size_t d = path.size() - 1;
			const frame& f = path.back();
			if (d < key.size()) {
				std::size_t i = f.n.below(std::uint8_t(key[d]));
				if (i != f.n.count) {
					transition t = f.n.at(i);
					std::string prefix(key.substr(0, d));
					prefix.push_back(char(t.label));
					return max_from(load(t.target), std::move(prefix), f.out + t.output);
				}
				if (f.n.is_final) { return std::pair{std::string(key.substr(0, d)), f.out + f.n.final_output}; }
			}
			path.pop_back();
		}
		return std::nullopt;
	}

	// Calls f(key, value) for each key in [lo, hi) in ascending order; an empty hi means no
	// upper bound. f returns false to stop.
	template <typename F>
	void range(std::string_view lo, std::string_view hi, F&& f) const {
		std::string key;
		walk(load(root_), 0, key, lo, true, hi, f);
	}

	template <typename F>
	void for_each(F&& f) const {
		range({}, {}, std::forward<F>(f));
	}

	// Calls f(key, value) for each key the automaton accepts, in ascending order. The
	// automaton provides start(), accept(state, byte), is_match(state), can_match(state).
	template <typename A, typename F>
	void intersect(const A& automaton, F&& f) const {
		std::string key;
		intersect_from(load(root_), 0, automaton, automaton.start(), key, f);
	}

private:
	std::optional<std::pair<std::string, std::uint64_t>> max_from(node n, std::string prefix, std::uint64_t out) const {
		while (n.count) {
			transition t = n.at(n.count - 1);
			prefix.push_back(char(t.label));
			out += t.output;
			n = load(t.target);
		}
		return std::pair{std::move(prefix), out + n.final_output};
	}

	// Depth-first in label order. on_lo: key is still a prefix of lo, so smaller labels are skipped.
	template <typename F>
	bool walk(const node& n, std::uint64_t out, std::string& key, std::string_view lo, bool on_lo, std::string_view hi, F& f) const {
		const std::size_t depth = key.size();
		if (n.is_final && !(on_lo && depth < lo.size())) {
			if (!hi.empty() && std::string_view(key) >= hi) { return false; }
			if (!f(std::string_view(key), out + n.final_output)) { return false; }
		}
		std::size_t i = 0;
		if (on_lo && depth < lo.size()) {
			while (i < n.count && n.at(i).label < std::uint8_t(lo[depth])) { ++i; }
		}
		for (; i < n.count; ++i) {
			transition t = n.at(i);
			key.push_back(char(t.label));
			// Everything under key is >= key; stop once key itself reaches hi.
			if (!hi.empty() && std::string_view(key) >= hi) {
				key.pop_back();
				return false;
			}
			bool still_lo = on_lo && depth < lo.size() && t.label == std::uint8_t(lo[depth]);
			bool go_on = walk(load(t.target), out + t.output, key, lo, still_lo, hi, f);
			key.pop_back();
			if (!go_on) { return false; }
		}
		return true;
	}

	template <typename A, typename S, typename F>
	bool intersect_from(const node& n, std::uint64_t out, const A& a, const S& state, std::string& key, F& f) const {
		if (n.is_final && a.is_match(state)) {
			if (!f(std::string_view(key), out + n.final_output)) { return false; }
		}
		for (std::size_t i = 0; i < n.count; ++i) {
			transition t = n.at(i);
			S next = a.accept(state, t.label);
			if (!a.can_match(next)) { continue; }
			key.push_back(char(t.label));
			bool go_on = intersect_from(load(t.target), out + t.output, a, next, key, f);
			key.pop_back();
			if (!go_on) { return false; }
		}
		return true;
	}

	std::span<const std::uint8_t> bytes_;
	std::uint64_t root_ = 0;
	std::uint64_t size_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Builder

// Builds an FST from keys inserted in strictly increasing byte order.
class builder {
public:
	builder() : out_("FST1", 4) { stack_.emplace_back(); }

	void insert(std::string_view key, std::uint64_t value) {
		if (count_ && key <= std::string_view(last_)) { throw std::invalid_argument("fst: keys must be strictly increasing"); }
		std::size_t prefix = 0;
		while (prefix < key.size() && prefix < last_.size() && key[prefix] == last_[prefix]) { ++prefix; }
		freeze_after(prefix);

		// Push outputs down the shared prefix: each transition keeps the common part.
		for (std::size_t d = 0; d < prefix; ++d) {
			auto& t = stack_[d].arcs.back();
			std::uint64_t common = std::min(t.output, value);
			std::uint64_t rest = t.output - common;
			if (rest) {
				auto& child = stack_[d + 1];
				for (auto& a : child.arcs) { a.output += rest; }
				if (child.is_final) { child.final_output += rest; }
			}
			t.output = common;
			value -= common;
		}
		if (key.empty()) {  // only possible as the first key
			stack_[0].is_final = true;
			stack_[0].final_output = value;
			++count_;
			return;
		}
		// New suffix nodes.
		for (std::size_t d = prefix; d < key.size(); ++d) {
			stack_[d].arcs.push_back({std::uint8_t(key[d]), d == prefix ? value : 0, 0});
			stack_.resize(std::max(stack_.size(), d + 2));
			stack_[d + 1] = pending{};
		}
		stack_[key.size()].is_final = true;
		last_.assign(key);
		++count_;
	}

	// Writes the root and the trailer; returns the FST bytes.
	std::string finish() {
		freeze_after(0);
		std::uint64_t root = write(stack_[0]);
		detail::put_le(out_, root, 8);
		detail::put_le(out_, count_, 8);
		return std::move(out_);
	}

private:
	struct arc { std::uint8_t label; std::uint64_t output; std::uint64_t target; };
	struct pending {
		std::vector<arc> arcs;
		bool is_final = false;
		std::uint64_t final_output = 0;
	};

	// Freezes nodes deeper than depth along the previous key, bottom up.
	void freeze_after(std::size_t depth) {
		for (std::size_t d = last_.size(); d > depth; --d) {
			std::uint64_t address = write(stack_[d]);
			stack_[d - 1].arcs.back().target = address;
			stack_[d] = pending{};
		}
	}

	struct signature_hash {
		using is_transparent = void;
		std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
	};

	// Returns the address of an equal node already written, or writes this one.
	std::uint64_t write(const pending& n) {
		std::string& sig = signature_;
		sig.clear();
		sig.push_back(char(n.is_final));
		detail::put_varint(sig, n.final_output);
		for (const auto& a : n.arcs) {
			sig.push_back(char(a.label));
			detail::put_varint(sig, a.output);
			detail::put_varint(sig, a.target);
		}
		if (auto it = registry_.find(std::string_view(sig)); it != registry_.end()) { return it->second; }

		const std::uint64_t address = out_.size();
		std::uint8_t h = (n.is_final ? detail::final_bit : 0) | (n.final_output ? detail::final_output_bit : 0);
		if (n.arcs.size() == 1) {
			const arc& a = n.arcs[0];
			out_.push_back(char(h | detail::chain_bit | (a.output ? detail::chain_output_bit : 0)));
			if (n.final_output) { detail::put_varint(out_, n.final_output); }
			out_.push_back(char(a.label));
			if (a.output) { detail::put_varint(out_, a.output); }
			detail::put_varint(out_, address - a.target);
		} else {
			const std::size_t count = n.arcs.size();
			out_.push_back(char(h | (std::min<std::size_t>(count, 31) << 3)));
			if (n.final_output) { detail::put_varint(out_, n.final_output); }
			if (count >= 31) { detail::put_varint(out_, count - 31); }
			if (count) {
				std::uint64_t max_out = 0, max_delta = 0;
				for (const auto& a : n.arcs) {
					out_.push_back(char(a.label));
					max_out = std::max(max_out, a.output);
					max_delta = std::max(max_delta, address - a.target);
				}
				unsigned ob = detail::bytes_for(max_out), tb = detail::bytes_for(max_delta);
				out_.push_back(char(ob | tb << 4));
				for (const auto& a : n.arcs) { detail::put_le(out_, a.output, ob); }
				for (const auto& a : n.arcs) { detail::put_le(out_, address - a.target, tb); }
			}
		}
		registry_.emplace(sig, address);
		return address;
	}

	std::string out_;
	std::vector<pending> stack_;  // stack_[d]: the unfrozen node at depth d of the last key
	std::string last_;
	std::uint64_t count_ = 0;
	std::unordered_map<std::string, std::uint64_t, signature_hash, std::equal_to<>> registry_;
	std::string signature_;  // reused buffer for registry lookups
};

// ---------------------------------------------------------------------------------------------
// Automata

// Accepts exactly the keys starting with a prefix.
struct prefix_automaton {
	std::string_view prefix;
	using state = std::size_t;  // bytes of prefix matched; npos = dead
	static constexpr state dead = ~std::size_t(0);
	state start() const noexcept { return 0; }
	state accept(state s, std::uint8_t b) const noexcept {
		if (s == dead) { return dead; }
		if (s >= prefix.size()) { return s; }
		return std::uint8_t(prefix[s]) == b ? s + 1 : dead;
	}
	bool is_match(state s) const noexcept { return s != dead && s >= prefix.size(); }
	bool can_match(state s) const noexcept { return s != dead; }
};

}  // namespace fst
