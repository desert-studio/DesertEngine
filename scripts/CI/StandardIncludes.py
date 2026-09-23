#!/usr/bin/env python3
"""A STANDARD SYMBOL MUST BE ANSWERED FOR BY THIS REPOSITORY'S OWN TEXT.

WHY THIS EXISTS. `sky_panorama_test.cpp` called `std::clamp` with only <cmath> included. libc++
declares <algorithm> from inside <cmath>'s include chain and MSVC's standard library does not, so the
file compiled on macOS and on every local sweep and failed on Windows Debug -- and the missing
overload made the enclosing `std::acos` ambiguous as well, so ONE absent include printed three errors
and named `clamp` in only two of them. It landed while the format gate was red, and Windows and macOS
both `needs: format`, so for days nothing ran that could see it. A skipped platform does not cost a
red build; it costs a build nobody runs.

THE RELATION PINNED HERE is not "the count of missing includes is zero" -- that is satisfiable by
editing a number. It is:

    for every row of the REGISTER below, and every file tracked in this repository under the scanned
    roots: if the file's source text uses one of the row's symbols, then the row's header is named by
    a `#include` in that file or in some header OF THIS REPOSITORY that the file reaches.

The second half of that sentence is the whole design. Our own headers are visible in our own text, so
a symbol reached through one of them is reached identically on every platform. The standard library's
headers are NOT: the standard guarantees nothing about one standard header including another, which
is exactly the freedom libc++ used and MSVC did not. So a use whose only supplier is a standard or
third-party header is a use this repository cannot answer for, and that is what is reported.

WHAT IT ASKS, AND OF WHAT. It asks the SOURCE TEXT, never a compiler: no run of any compiler on a
macOS machine can see this defect class, which is the property that let it live. The file list comes
from `git ls-files`, not from walking the disk -- two instruments in this repository have already been
green because of files that existed on one machine and in no checkout. And it runs on ubuntu in the
`format` job, where neither Homebrew nor MSVC exists, so it cannot satisfy itself from a runner's own
prefix.

THREE ANSWERS, THREE EXIT CODES. "I found no violation" and "I could not look" must never share one.
    0  scanned, no violation
    1  scanned, violations printed
    2  COULD NOT SCAN -- the gate is broken, not the sources
An empty successful answer is the most dangerous output an instrument has, so the preconditions below
are checked before the scan and reported as 2: the roots must be tracked and non-empty, and every
register row must be SIGHTED -- some file in the tree must use its symbols, satisfied or not. A row
that sees nothing at all in fourteen hundred files is a scanner that is not reading the tree, and it
says so rather than passing in silence.

WHAT IS DEFERRED, BY NAME AND WITH ITS REASON. Two rows are deliberately NOT in the register:

  * <cstddef> for `size_t` / `ptrdiff_t` (122 files on 2026-09-23)
  * <utility> for `std::move` / `std::forward` / `std::pair` / `std::swap` (94 files)

They are deferred, not judged harmless. The reason is the ratio: 216 of the 298 findings a full
register produces are those two, every standard container and string header on both implementations
supplies them (MSVC through <vcruntime.h> and <xmemory>, which <vector>, <string> and <memory> all
reach), and this repository has no instance of either ever failing on any platform. THE RETURN
CONDITION IS NAMED: the first Windows error naming `size_t`, `std::move`, `std::forward`, `std::pair`
or `std::swap` puts the row into the register and the 216 files into a task. Until then the deferral
is visible here rather than absent from the list.

LIMITS, SO NOBODY READS MORE INTO A GREEN THAN IT SAYS.
  * Symbols are matched by pattern over comment-stripped and literal-stripped text. Unqualified calls
    found by ADL, macro-built names and `using namespace std;` are outside what this can see.
  * `#if` is not evaluated: a use inside a disabled branch still counts, which is the safe direction.
  * Reachability is textual, ignoring include guards and conditionals -- also the safe direction for
    the supplier and the generous one for the file, so a green here is weaker than a compile and a red
    here is a fact.
"""

import os
import re
import subprocess
import sys
from collections import defaultdict

# ── THE REGISTER. Named rows. The count of violations is DERIVED from it, never written down. ──────
#
# Each row is  name -> (header, [patterns]).  A row's header may also be satisfied by any of the
# ALIASES below it (the C header a C++ header subsumes).
REGISTER = {
    "algorithm": (
        "algorithm",
        [
            r"std::sort\b", r"std::stable_sort\b", r"std::partial_sort\b", r"std::nth_element\b",
            r"std::find\b", r"std::find_if\b", r"std::find_if_not\b", r"std::find_first_of\b",
            r"std::min_element\b", r"std::max_element\b", r"std::minmax_element\b",
            r"std::clamp\b", r"std::copy\b", r"std::copy_n\b", r"std::copy_if\b",
            r"std::transform\b", r"std::remove\b", r"std::remove_if\b",
            r"std::fill\b", r"std::fill_n\b", r"std::count\b", r"std::count_if\b",
            r"std::any_of\b", r"std::all_of\b", r"std::none_of\b",
            r"std::lower_bound\b", r"std::upper_bound\b", r"std::binary_search\b",
            r"std::equal_range\b", r"std::reverse\b", r"std::rotate\b", r"std::shuffle\b",
            r"std::partition\b", r"std::stable_partition\b", r"std::for_each\b",
            r"std::search\b", r"std::equal\b", r"std::lexicographical_compare\b",
            r"std::max\b", r"std::min\b", r"std::minmax\b",
            r"std::generate\b", r"std::generate_n\b", r"std::includes\b",
            r"std::set_difference\b", r"std::set_union\b", r"std::set_intersection\b",
            r"std::iter_swap\b", r"std::swap_ranges\b", r"std::mismatch\b",
            r"std::replace\b", r"std::replace_if\b", r"std::is_sorted\b",
            r"std::next_permutation\b", r"std::unique\b(?!_ptr)",
        ],
    ),
    "numeric": (
        "numeric",
        [
            r"std::accumulate\b", r"std::iota\b", r"std::inner_product\b", r"std::partial_sum\b",
            r"std::reduce\b", r"std::gcd\b", r"std::lcm\b", r"std::midpoint\b",
            r"std::adjacent_difference\b", r"std::transform_reduce\b",
        ],
    ),
    "limits": ("limits", [r"std::numeric_limits\b"]),
    "cstring": (
        "cstring",
        [
            r"\bstd::memcpy\b", r"\bstd::memset\b", r"\bstd::memcmp\b", r"\bstd::memmove\b",
            r"\bstd::strlen\b", r"\bstd::strcmp\b", r"\bstd::strncmp\b", r"\bstd::strcpy\b",
            r"(?<![\w:.>])memcpy\s*\(", r"(?<![\w:.>])memset\s*\(", r"(?<![\w:.>])memcmp\s*\(",
            r"(?<![\w:.>])memmove\s*\(", r"(?<![\w:.>])strlen\s*\(", r"(?<![\w:.>])strcmp\s*\(",
            r"(?<![\w:.>])strncmp\s*\(",
        ],
    ),
    "cstdint": (
        "cstdint",
        [
            r"(?<![\w:])(?:std::)?uint8_t\b", r"(?<![\w:])(?:std::)?uint16_t\b",
            r"(?<![\w:])(?:std::)?uint32_t\b", r"(?<![\w:])(?:std::)?uint64_t\b",
            r"(?<![\w:])(?:std::)?int8_t\b", r"(?<![\w:])(?:std::)?int16_t\b",
            r"(?<![\w:])(?:std::)?int32_t\b", r"(?<![\w:])(?:std::)?int64_t\b",
            r"(?<![\w:])(?:std::)?uintptr_t\b", r"(?<![\w:])(?:std::)?intptr_t\b",
            r"(?<![\w:])(?:std::)?uintmax_t\b",
        ],
    ),
    "functional": (
        "functional",
        [
            r"std::function\b", r"std::bind\b", r"std::hash\b", r"std::ref\b", r"std::cref\b",
            r"std::greater\b", r"std::less\b", r"std::plus\b", r"std::equal_to\b",
            r"std::reference_wrapper\b", r"std::invoke\b",
        ],
    ),
    "memory": (
        "memory",
        [
            r"std::unique_ptr\b", r"std::shared_ptr\b", r"std::weak_ptr\b",
            r"std::make_unique\b", r"std::make_shared\b", r"std::enable_shared_from_this\b",
            r"std::static_pointer_cast\b", r"std::dynamic_pointer_cast\b", r"std::addressof\b",
        ],
    ),
    "string": (
        "string",
        [
            r"std::string\b(?!_view|stream)", r"std::to_string\b", r"std::stoi\b",
            r"std::stof\b", r"std::stod\b", r"std::stoul\b", r"std::getline\b",
        ],
    ),
    "string_view": ("string_view", [r"std::string_view\b"]),
    "vector": ("vector", [r"std::vector\b"]),
    "array": ("array", [r"std::array\b"]),
    "map": ("map", [r"std::map\b", r"std::multimap\b"]),
    "unordered_map": ("unordered_map", [r"std::unordered_map\b", r"std::unordered_multimap\b"]),
    "set": ("set", [r"std::set\b", r"std::multiset\b"]),
    "unordered_set": ("unordered_set", [r"std::unordered_set\b"]),
    "optional": ("optional", [r"std::optional\b", r"std::nullopt\b"]),
    "variant": ("variant", [r"std::variant\b", r"std::holds_alternative\b", r"std::get_if\b"]),
    "tuple": ("tuple", [r"std::tuple\b", r"std::make_tuple\b", r"std::tie\b", r"std::apply\b"]),
    "type_traits": (
        "type_traits",
        [
            r"std::is_same\b", r"std::enable_if\b", r"std::decay\b", r"std::remove_reference\b",
            r"std::is_base_of\b", r"std::conditional\b", r"std::underlying_type\b",
            r"std::is_integral\b", r"std::is_floating_point\b", r"std::true_type\b",
            r"std::false_type\b", r"std::integral_constant\b", r"std::void_t\b",
            r"std::is_enum\b", r"std::is_pointer\b", r"std::is_reference\b", r"std::is_const\b",
            r"std::is_convertible\b", r"std::is_constructible\b", r"std::is_trivially_copyable\b",
            r"std::remove_cv\b", r"std::remove_pointer\b", r"std::add_pointer\b",
            r"std::invoke_result\b", r"std::common_type\b", r"std::is_signed\b",
            r"std::is_unsigned\b", r"std::is_class\b", r"std::is_arithmetic\b", r"std::is_void\b",
        ],
    ),
    "cmath": (
        "cmath",
        [
            r"std::sqrt\b", r"std::sin\b", r"std::cos\b", r"std::tan\b", r"std::atan2\b",
            r"std::acos\b", r"std::asin\b", r"std::pow\b", r"std::fabs\b", r"std::floor\b",
            r"std::ceil\b", r"std::round\b", r"std::lround\b", r"std::log\b", r"std::exp\b",
            r"std::fmod\b", r"std::isnan\b", r"std::isinf\b", r"std::isfinite\b",
            r"std::trunc\b", r"std::hypot\b",
        ],
    ),
}

# A C++ header is also satisfied by the C header it subsumes, and vice versa where the tree uses it.
ALIASES = {
    "cstring": ("string.h",),
    "cstdint": ("stdint.h",),
    "cmath": ("math.h",),
}

# Roots whose tracked sources are scanned. Tools/ is in: its mains are compiled by the same makefiles.
SCAN_ROOTS = ("Desert", "Editor", "Runtime", "Tools")

# Where an `#include <...>` of OUR OWN headers resolves from -- the projects' premake `includedirs`,
# never `externalincludedirs` (those are ThirdParty, which by construction supplies nothing here).
# `Desert/Common/Source/Common` is a root in its own right: Common/premake5.lua lists both "Source/"
# and "Source/Common", which is why `#include <Core/Layer.hpp>` resolves. Missing a root would make
# the gate STRICTER than intended rather than blinder, but it would also misreport where a symbol
# comes from, so they are listed rather than guessed.
INCLUDE_ROOTS = (
    "Desert/Desert/Source",
    "Desert/Common/Source",
    "Desert/Common/Source/Common",
    "Editor/Source",
    "Runtime/Source",
    "Tools/SceneMigrator/Source",
)

SOURCE_EXTS = (".cpp", ".hpp", ".h", ".cc", ".inl")

RAW_STRING = re.compile(r'R"([^(\s]*)\((.*?)\)\1"', re.S)
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
STRING_LIT = re.compile(r'"(?:\\.|[^"\\\n])*"')
CHAR_LIT = re.compile(r"'(?:\\.|[^'\\\n])'")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.M)


def fail_to_scan(*lines):
    print("CANNOT SCAN -- this is a gate defect, not a source defect:")
    for line in lines:
        print(f"  {line}")
    sys.exit(2)


def tracked_sources(repo):
    """The files THIS REPOSITORY contains, asked of git rather than of the disk.

    A working directory can hold files no checkout has (and can be missing files every checkout has);
    both have already produced a green instrument here. `git ls-files -z` answers about the index.

    The LIST comes from git; the CONTENT is then read from the working tree, deliberately. The
    repository-level fact that has burned this project is a file being present locally and absent from
    every checkout, and that is a question about the list. Reading content from the working tree is
    what a developer running this before committing wants, and on a CI checkout the two are the same
    bytes. A tracked file missing from the working tree is exit 2, not a silent skip.
    """
    try:
        out = subprocess.run(
            ["git", "-C", repo, "ls-files", "-z", "--"] + list(SCAN_ROOTS),
            capture_output=True,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        fail_to_scan(f"`git ls-files` failed: {exc}", "The file list must come from the index.")
    names = [n for n in out.decode("utf-8", "replace").split("\0") if n]
    if not names:
        fail_to_scan(
            "`git ls-files` returned nothing for " + ", ".join(SCAN_ROOTS),
            "Either this is not the repository's root or the checkout is sparse.",
        )
    return sorted(
        n for n in names if n.endswith(SOURCE_EXTS) and "/ThirdParty/" not in "/" + n
    )


def _blank_keeping_lines(match):
    """Erase a span but keep its newlines, so a reported file:line is the line in the FILE.

    A block comment or raw string spanning twenty lines replaced by one space moves every line number
    below it, and a gate that names the wrong line gets argued with instead of fixed.
    """
    return "\n" * match.group(0).count("\n")


def strip_noncode(text):
    text = RAW_STRING.sub(_blank_keeping_lines, text)
    text = BLOCK_COMMENT.sub(_blank_keeping_lines, text)
    text = LINE_COMMENT.sub(" ", text)
    text = STRING_LIT.sub('""', text)
    return CHAR_LIT.sub("' '", text)


class Tree:
    def __init__(self, repo, files):
        self.repo = repo
        self.files = files
        self.fileset = set(files)
        self.by_rootrel = {}
        for f in files:
            for r in INCLUDE_ROOTS:
                if f.startswith(r + "/"):
                    self.by_rootrel.setdefault(f[len(r) + 1:], f)
        self._text = {}
        self._inc = {}
        self._closure = {}
        self.resolved_local = 0

    def text(self, f):
        if f not in self._text:
            try:
                with open(os.path.join(self.repo, f), encoding="utf-8", errors="replace") as fh:
                    self._text[f] = fh.read()
            except OSError as exc:
                fail_to_scan(f"{f} is tracked but unreadable: {exc}")
        return self._text[f]

    def includes(self, f):
        """(header names this file cannot resolve inside the repo, repo-local files it includes)."""
        if f in self._inc:
            return self._inc[f]
        outside, local = set(), []
        for kind, name in INCLUDE_RE.findall(self.text(f)):
            cand = None
            if kind == '"':
                p = os.path.normpath(os.path.join(os.path.dirname(f), name))
                if p in self.fileset:
                    cand = p
            if cand is None and name in self.by_rootrel:
                cand = self.by_rootrel[name]
            if cand is None:
                p = os.path.normpath(os.path.join(os.path.dirname(f), name))
                if p in self.fileset:
                    cand = p
            if cand is None:
                outside.add(name)
            else:
                local.append(cand)
                self.resolved_local += 1
        self._inc[f] = (outside, local)
        return self._inc[f]

    def supplied(self, f):
        """Every header named from this file or from any header OF THIS REPOSITORY it reaches."""
        if f in self._closure:
            return self._closure[f]
        acc, seen, stack = set(), set(), [f]
        while stack:
            cur = stack.pop()
            if cur in seen:
                continue
            seen.add(cur)
            outside, local = self.includes(cur)
            acc |= outside
            stack.extend(local)
        self._closure[f] = acc
        return acc


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

    if not REGISTER:
        fail_to_scan("The register is empty; there is nothing to enforce.")
    compiled = {}
    for name, (header, pats) in REGISTER.items():
        try:
            compiled[name] = (header, [re.compile(p) for p in pats])
        except re.error as exc:
            fail_to_scan(f"register row '{name}' has an unusable pattern: {exc}")

    files = tracked_sources(repo)
    for root in SCAN_ROOTS:
        if not any(f.startswith(root + "/") for f in files):
            fail_to_scan(f"no tracked source files under {root}/ -- the scan would be vacuous.")

    violations = []          # (file, row, header, symbol, line)
    sightings = defaultdict(int)   # row -> how many files use its symbols at all

    tree = Tree(repo, files)
    for f in files:
        code = strip_noncode(tree.text(f))
        supplied = None
        for row, (header, pats) in compiled.items():
            hit = None
            for p in pats:
                m = p.search(code)
                if m:
                    hit = m
                    break
            if hit is None:
                continue
            sightings[row] += 1
            if supplied is None:
                supplied = tree.supplied(f)
            if header in supplied or any(a in supplied for a in ALIASES.get(row, ())):
                continue
            violations.append((f, row, header, hit.group(0), code[: hit.start()].count("\n") + 1))

    # ── preconditions, checked AFTER the pass because they are about what the pass saw ─────────────
    if tree.resolved_local == 0:
        fail_to_scan(
            "not one `#include` in the tree resolved to a file of this repository.",
            "INCLUDE_ROOTS no longer matches the project's includedirs, so every file would look as",
            "though it includes nothing of ours -- a far stricter question than the one asked here.",
        )
    blind = [row for row in compiled if sightings[row] == 0]
    if blind:
        fail_to_scan(
            f"{len(files)} files were read and these register rows saw no use of ANY of their",
            "symbols, which reads as a scanner that is not reading the tree rather than as a tree",
            "that does not use them: " + ", ".join(sorted(blind)),
        )

    if violations:
        print("STANDARD SYMBOL WITH NO SUPPLIER IN THIS REPOSITORY")
        print("")
        print("Each line uses a symbol whose header is named neither by the file nor by any header of")
        print("this repository it includes. It compiles only where the platform's standard library")
        print("happens to include that header from another one -- libc++ does, MSVC does not.")
        print("Fix: add the `#include <...>` to the file itself.")
        print("")
        for f, row, header, sym, line in sorted(violations):
            print(f"  {f}:{line}: uses `{sym}` -- add #include <{header}>   [row {row}]")
        print("")
        print(f"{len(violations)} use(s) in {len({v[0] for v in violations})} file(s).")
        return 1

    print(
        f"standard includes: OK ({len(compiled)} register row(s) over {len(files)} tracked source "
        f"file(s); {tree.resolved_local} include(s) resolved inside the repository)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
