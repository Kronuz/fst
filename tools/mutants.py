#!/usr/bin/env python3
"""Breaks fst.hh in a set of ways and requires the tests to fail on each.

    python3 tools/mutants.py [same_node|registry|walk|all] [--also-repo]

Run it from the repository root. CXX picks the compiler (default c++), JOBS the parallelism and TIMEOUT
the seconds a test may run: a test that hangs on a mutant, as the walk that never advances does, has
failed on it. A full run builds about 40 test binaries.

Each mutant is one text edit of fst.hh, built with the test that has to notice it:

  same_node  every comparison of same_node() removed, and "every hit is equal"; test/collide.cc,
             built with FST_TEST_HASH_MASK=0 so that same_node() decides every registry hit
  registry   the switch from 8-byte to 16-byte registry slots and the wide table's growth;
             test/diff.cc, built with FST_TEST_NARROW_LIMIT=70000
  walk       for_each emitting no key, ignoring the stop, dropping a transition's output or
             never advancing; test/diff.cc and test/test.cc

A mutant that no required test fails on, or that does not build, is reported as NOT KILLED and the
exit status is 1.
--also-repo adds test/diff.cc and test/test.cc to the same_node run as informational columns:
they let most same_node() mutants through, which is why test/collide.cc exists.
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
SRC = open(os.path.join(ROOT, "fst.hh")).read()
CXX = os.environ.get("CXX", "c++")
JOBS = int(os.environ.get("JOBS", "4"))
TIMEOUT = int(os.environ.get("TIMEOUT", "60"))  # seconds; a test that hangs on a mutant counts as killing it

SAME_NODE = {
	"all_true": ("bool same_node(std::uint64_t address, const pending& n) const noexcept {\n",
	             "bool same_node(std::uint64_t address, const pending& n) const noexcept {\n\t\treturn true;\n"),
	"final_flag": ("if (bool(h & detail::final_bit) != n.is_final) { return false; }", ""),
	"final_output": ("if (n.final_output != ((h & detail::final_output_bit) ? detail::get_varint(p) : 0)) { return false; }",
	                 "if ((h & detail::final_output_bit)) { detail::get_varint(p); }"),
	"chain_arcs": ("if (n.arcs.size() != 1) { return false; }", ""),
	"chain_label": ("if (*p++ != a.label) { return false; }", "++p;"),
	"chain_output": ("if (a.output != ((h & detail::chain_output_bit) ? detail::get_varint(p) : 0)) { return false; }",
	                 "if ((h & detail::chain_output_bit)) { detail::get_varint(p); }"),
	"chain_target": ("return address - detail::get_varint(p) == a.target;", "detail::get_varint(p); return true;"),
	"count": ("if (count != n.arcs.size()) { return false; }", ""),
	"general_label": ("if (labels[i] != a.label || exact_le(p + i * ob, ob) != a.output ||",
	                  "if (exact_le(p + i * ob, ob) != a.output ||"),
	"general_output": ("if (labels[i] != a.label || exact_le(p + i * ob, ob) != a.output ||\n\t\t\t    address - exact_le(p + count * ob + i * tb, tb) != a.target) { return false; }",
	                   "if (labels[i] != a.label ||\n\t\t\t    address - exact_le(p + count * ob + i * tb, tb) != a.target) { return false; }"),
	"general_target": ("if (labels[i] != a.label || exact_le(p + i * ob, ob) != a.output ||\n\t\t\t    address - exact_le(p + count * ob + i * tb, tb) != a.target) { return false; }",
	                   "if (labels[i] != a.label || exact_le(p + i * ob, ob) != a.output) { return false; }"),
}

REGISTRY = {
	"widen_drops_entries": ("\t\t\twide_[i] = {s.tag, s.address};\n", "\t\t\t(void)i;\n"),
	"wide_never_registers": ("\t\twide_[at] = {tag, address};\n\t\t++used_;", "\t\t++used_;"),
	"narrow_never_grows": ("\t\tif (used_ * 2 >= narrow_.size()) { grow(narrow_); }", "\t\tif (used_ * 2 >= narrow_.size() * 2) { grow(narrow_); }"),
	"wide_grow_skipped": ("\t\t\tgrow(wide_);\n", "\t\t\t(void)0;\n"),
	"wide_check_dropped": ("\t\tif (!wide_.empty()) [[unlikely]] { return write_wide(hash, n); }", "\t\tif (false) { return write_wide(hash, n); }"),
	"room_never_widens": ("\t\tif (out_.size() + check_every * max_node_bytes > narrow_limit) {\n\t\t\twiden();\n\t\t\treturn false;\n\t\t}\n", ""),
}

WALK = {
	"for_each_emits_no_key": ("\t\t\t\tbool is_key = true;", "\t\t\t\tbool is_key = false;"),
	"walk_ignores_stop": ("\t\t\t\t\tif (!f(path, out + final_out)) { return false; }", "\t\t\t\t\t(void)f(path, out + final_out);"),
	"walk_drops_output": ("\t\t\tconst std::uint64_t out = top.out + t_out;", "\t\t\tconst std::uint64_t out = top.out;"),
	"walk_never_advances": ("\t\t\t++top.next;\n\t\t\tconst std::size_t depth = stack.size() - 1;", "\t\t\tconst std::size_t depth = stack.size() - 1;"),
}

FIXTURES = ["-DFST_FIXTURES=\"%s/test/fixtures\"" % ROOT, "-pthread"]
COLLIDE = ("collide", "test/collide.cc", ["-DFST_TEST_HASH_MASK=0"])
DIFF = ("diff", "test/diff.cc", [])
DIFF_WIDE = ("diff_wide", "test/diff.cc", ["-DFST_TEST_NARROW_LIMIT=70000"])
TEST = ("test", "test/test.cc", FIXTURES)

# group name -> (mutants, required tests, informational tests with --also-repo)
GROUPS = {
	"same_node": (SAME_NODE, [COLLIDE], [DIFF, TEST]),
	"registry": (REGISTRY, [DIFF_WIDE], []),
	"walk": (WALK, [DIFF, TEST], []),
}


def run(command):
	try:
		return subprocess.run(command, capture_output=True, text=True, timeout=TIMEOUT)
	except subprocess.TimeoutExpired:
		return None


def verdict(directory, target):
	"""'killed', 'SURVIVED' or 'ERROR ...' for one test built against one mutant header."""
	name, source, defines = target
	exe = os.path.join(directory, name)
	built = run([CXX, "-std=c++20", "-O2", "-I" + directory, "-I" + os.path.join(ROOT, "test")] + defines + [os.path.join(ROOT, source), "-o", exe])
	if built is None or built.returncode:
		return "ERROR compile " + (built.stderr[:200] if built else "timed out")
	result = run([exe])
	if result is None or result.returncode != 0:
		return "killed"
	return "SURVIVED"


def check(work, group, name, edit, required, extra):
	old, new = edit
	if old not in SRC:
		return name, None, ["ERROR the text to replace is not in fst.hh"]
	directory = os.path.join(work, group, name)
	os.makedirs(directory)
	with open(os.path.join(directory, "fst.hh"), "w") as header:
		header.write(SRC.replace(old, new, 1))
	columns = [(t[0], verdict(directory, t)) for t in required]
	columns += [(t[0] + " (informational)", verdict(directory, t)) for t in extra]
	return name, [v for _, v in columns[:len(required)]], ["%s: %s" % c for c in columns]


def main():
	args = [a for a in sys.argv[1:] if not a.startswith("--")]
	chosen = args[0] if args else "all"
	if chosen != "all" and chosen not in GROUPS:
		sys.exit("usage: mutants.py [%s|all] [--also-repo]" % "|".join(GROUPS))
	also_repo = "--also-repo" in sys.argv
	failed = False
	work = tempfile.mkdtemp(prefix="fst-mutants-")
	try:
		for group, (mutants, required, extra) in GROUPS.items():
			if chosen not in ("all", group):
				continue
			print("== %s (%d mutants; tests: %s)" % (group, len(mutants), ", ".join(t[0] for t in required)), flush=True)
			with concurrent.futures.ThreadPoolExecutor(max_workers=JOBS) as pool:
				futures = [pool.submit(check, work, group, n, edit, required, extra if also_repo else []) for n, edit in mutants.items()]
				for future in futures:
					name, required_verdicts, columns = future.result()
					bad = required_verdicts is None or any(v.startswith("ERROR") for v in required_verdicts) or "killed" not in required_verdicts
					failed = failed or bad
					print("%-24s %s%s" % (name, "  ".join(columns), "   <-- NOT KILLED" if bad else ""), flush=True)
	finally:
		shutil.rmtree(work, ignore_errors=True)
	print("some mutants were not killed" if failed else "all mutants killed")
	sys.exit(1 if failed else 0)


if __name__ == "__main__":
	main()
