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

// Longest key the builder accepts and a validated map can hold; bounds traversal depth.
inline constexpr std::size_t max_key_length = 4096;

// Tag for map's constructor: skip validation of bytes already known good.
struct trusted_t { explicit trusted_t() = default; };
inline constexpr trusted_t trusted{};

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
	// Validates the bytes (one pass over every node); throws std::invalid_argument when they
	// are not a well-formed FST, so lookups on any accepted input stay in bounds.
	explicit map(std::span<const std::uint8_t> bytes) : map(bytes, trusted) { validate(); }
	explicit map(std::string_view bytes) : map(as_bytes(bytes)) {}

	// Skips validation, for bytes that already passed it (for example, checksummed on disk).
	map(std::span<const std::uint8_t> bytes, trusted_t) : bytes_(bytes) {
		if (bytes.size() < 21 || std::memcmp(bytes.data(), "FST1", 4) != 0) { throw std::invalid_argument("fst: not an FST"); }
		root_ = detail::get_le(bytes.data() + bytes.size() - 16, 8);
		size_ = detail::get_le(bytes.data() + bytes.size() - 8, 8);
	}
	map(std::string_view bytes, trusted_t) : map(as_bytes(bytes), trusted) {}

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

	// The value of the largest key <= key, without materializing that key: one pass
	// down the key plus one descent to a subtree's maximum. No allocation.
	std::optional<std::uint64_t> floor_value(std::string_view key) const noexcept {
		const std::uint8_t* base = bytes_.data();
		std::uint64_t out = 0, address = root_;
		// The deepest candidate below key: either a prefix that is a key (value known) or
		// the subtree under a smaller label (descend to its maximum at the end).
		enum class kind { none, value, subtree } best = kind::none;
		std::uint64_t best_value = 0, best_address = 0;
		for (std::size_t d = 0;; ++d) {
			const std::uint8_t* p = base + address;
			const std::uint8_t h = *p++;
			std::uint64_t final_out = (h & detail::final_output_bit) ? detail::get_varint(p) : 0;
			if (d == key.size()) {
				if (h & detail::final_bit) { return out + final_out; }
				break;  // every key below is longer, so greater
			}
			if (h & detail::final_bit) { best = kind::value; best_value = out + final_out; }
			const std::uint8_t c = std::uint8_t(key[d]);
			if (h & detail::chain_bit) {
				const std::uint8_t label = *p++;
				std::uint64_t t_out = (h & detail::chain_output_bit) ? detail::get_varint(p) : 0;
				std::uint64_t target = address - detail::get_varint(p);
				if (label < c) { best = kind::subtree; best_value = out + t_out; best_address = target; break; }
				if (label > c) { break; }
				out += t_out;
				address = target;
				continue;
			}
			std::size_t count = h >> 3;
			if (count == 31) { count += std::size_t(detail::get_varint(p)); }
			if (!count) { break; }
			const std::uint8_t* labels = p;
			p += count;
			const unsigned ob = *p & 0x0f, tb = *p >> 4;
			++p;
			std::size_t at = std::size_t(std::lower_bound(labels, labels + count, c) - labels);
			if (at > 0) {  // labels[at - 1] < c: a smaller subtree
				best = kind::subtree;
				best_value = out + detail::get_le(p + (at - 1) * ob, ob);
				best_address = address - detail::get_le(p + count * ob + (at - 1) * tb, tb);
			}
			if (at == count || labels[at] != c) { break; }
			out += detail::get_le(p + at * ob, ob);
			address -= detail::get_le(p + count * ob + at * tb, tb);
		}
		if (best == kind::none) { return std::nullopt; }
		if (best == kind::value) { return best_value; }
		// Maximum key under best_address: follow the last transition to a leaf.
		std::uint64_t v = best_value;
		for (address = best_address;;) {
			node n = load(address);
			if (!n.count) { return v + n.final_output; }
			transition t = n.at(n.count - 1);
			v += t.output;
			address = t.target;
		}
	}

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
	// Iterative depth-first walk in label order, so deep keys never grow the native stack.
	template <typename F>
	void range(std::string_view lo, std::string_view hi, F&& f) const {
		struct frame { node n; std::uint64_t out; std::size_t next; bool on_lo; };
		std::vector<frame> stack;
		std::string key;  // key.size() == stack.size() - 1 while walking
		// Visits a node: emits it if it is a key in range, then queues its transitions.
		// on_lo: key is still a prefix of lo, so smaller labels are skipped.
		auto enter = [&](const node& n, std::uint64_t out, bool on_lo) {
			const std::size_t depth = key.size();
			if (n.is_final && !(on_lo && depth < lo.size())) {
				if (!hi.empty() && std::string_view(key) >= hi) { return false; }
				if (!f(std::string_view(key), out + n.final_output)) { return false; }
			}
			std::size_t i = 0;
			if (on_lo && depth < lo.size()) {
				while (i < n.count && n.at(i).label < std::uint8_t(lo[depth])) { ++i; }
			}
			stack.push_back({n, out, i, on_lo});
			return true;
		};
		if (!enter(load(root_), 0, true)) { return; }
		while (!stack.empty()) {
			frame& top = stack.back();
			if (top.next == top.n.count) {
				stack.pop_back();
				if (!stack.empty()) { key.pop_back(); }
				continue;
			}
			const transition t = top.n.at(top.next++);
			const std::size_t depth = key.size();
			const bool still_lo = top.on_lo && depth < lo.size() && t.label == std::uint8_t(lo[depth]);
			const std::uint64_t out = top.out + t.output;
			key.push_back(char(t.label));
			// Everything under key is >= key; stop once key itself reaches hi.
			if (!hi.empty() && std::string_view(key) >= hi) { return; }
			if (!enter(load(t.target), out, still_lo)) { return; }
		}
	}

	template <typename F>
	void for_each(F&& f) const {
		range({}, {}, std::forward<F>(f));
	}

	// Calls f(key, value) for each key the automaton accepts, in ascending order. The
	// automaton provides start(), accept(state, byte), is_match(state), can_match(state).
	template <typename A, typename F>
	void intersect(const A& automaton, F&& f) const {
		using state_t = decltype(automaton.start());
		struct frame { node n; std::uint64_t out; std::size_t next; state_t state; };
		std::vector<frame> stack;
		std::string key;  // key.size() == stack.size() - 1 while walking
		auto enter = [&](const node& n, std::uint64_t out, state_t state) {
			if (n.is_final && automaton.is_match(state)) {
				if (!f(std::string_view(key), out + n.final_output)) { return false; }
			}
			stack.push_back({n, out, 0, std::move(state)});
			return true;
		};
		if (!enter(load(root_), 0, automaton.start())) { return; }
		while (!stack.empty()) {
			frame& top = stack.back();
			if (top.next == top.n.count) {
				stack.pop_back();
				if (!stack.empty()) { key.pop_back(); }
				continue;
			}
			const transition t = top.n.at(top.next++);
			state_t next = automaton.accept(top.state, t.label);
			if (!automaton.can_match(next)) { continue; }
			const std::uint64_t out = top.out + t.output;
			key.push_back(char(t.label));
			if (!enter(load(t.target), out, std::move(next))) { return; }
		}
	}

private:
	static std::span<const std::uint8_t> as_bytes(std::string_view s) noexcept {
		return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
	}

	[[noreturn]] static void malformed() { throw std::invalid_argument("fst: malformed bytes"); }

	// Parses every node in address order. Children precede parents, so each target must be
	// an already-seen node start below its parent; that also rules out cycles. Tracks each
	// node's longest key and key count to check depth and the trailer.
	void validate() const {
		const std::uint8_t* base = bytes_.data();
		const std::uint64_t end = bytes_.size() - 16;  // nodes end where the trailer starts
		if (root_ < 4 || root_ >= end) { malformed(); }
		std::vector<std::uint64_t> starts;    // node addresses, increasing
		std::vector<std::uint32_t> depth;     // longest key below each node
		std::vector<std::uint64_t> keys;      // keys below each node, saturating
		auto varint = [&](std::uint64_t& at) {
			std::uint64_t v = 0;
			for (unsigned shift = 0;; shift += 7) {
				if (at >= end || shift > 63) { malformed(); }
				std::uint8_t b = base[at++];
				v |= std::uint64_t(b & 0x7f) << shift;
				if (!(b & 0x80)) { return v; }
			}
		};
		auto target_index = [&](std::uint64_t address, std::uint64_t delta) {
			if (delta == 0 || delta > address - 4) { malformed(); }
			std::uint64_t target = address - delta;
			auto it = std::lower_bound(starts.begin(), starts.end(), target);
			if (it == starts.end() || *it != target) { malformed(); }
			return std::size_t(it - starts.begin());
		};
		auto add = [](std::uint64_t a, std::uint64_t b) { return a > ~0ull - b ? ~0ull : a + b; };
		std::uint64_t at = 4;
		while (at < end) {
			const std::uint64_t address = at;
			const std::uint8_t h = base[at++];
			if (h & detail::final_output_bit && !(h & detail::final_bit)) { malformed(); }
			if (h & detail::final_output_bit) { varint(at); }
			std::uint32_t d = 0;
			std::uint64_t k = (h & detail::final_bit) ? 1 : 0;
			if (h & detail::chain_bit) {
				if (h & 0xf0) { malformed(); }
				if (at >= end) { malformed(); }
				++at;  // label
				if (h & detail::chain_output_bit) { varint(at); }
				std::size_t t = target_index(address, varint(at));
				d = depth[t] + 1;
				k = add(k, keys[t]);
			} else {
				std::uint64_t count = h >> 3;
				if (count == 31) { count += varint(at); }
				if (count) {
					if (count > 256 || end - at < count + 1) { malformed(); }
					const std::uint8_t* labels = base + at;
					for (std::uint64_t i = 1; i < count; ++i) {
						if (labels[i] <= labels[i - 1]) { malformed(); }
					}
					at += count;
					const unsigned ob = base[at] & 0x0f, tb = base[at] >> 4;
					++at;
					if (ob > 8 || tb == 0 || tb > 8 || end - at < count * (ob + tb)) { malformed(); }
					const std::uint8_t* deltas = base + at + count * ob;
					for (std::uint64_t i = 0; i < count; ++i) {
						std::size_t t = target_index(address, detail::get_le(deltas + i * tb, tb));
						d = std::max(d, depth[t] + 1);
						k = add(k, keys[t]);
					}
					at += count * (ob + tb);
				}
			}
			if (d > max_key_length) { malformed(); }
			starts.push_back(address);
			depth.push_back(d);
			keys.push_back(k);
		}
		if (at != end || starts.empty() || starts.back() != root_ || keys.back() != size_) { malformed(); }
	}

	std::optional<std::pair<std::string, std::uint64_t>> max_from(node n, std::string prefix, std::uint64_t out) const {
		while (n.count) {
			transition t = n.at(n.count - 1);
			prefix.push_back(char(t.label));
			out += t.output;
			n = load(t.target);
		}
		return std::pair{std::move(prefix), out + n.final_output};
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
		if (key.size() > max_key_length) { throw std::invalid_argument("fst: key longer than max_key_length"); }
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
