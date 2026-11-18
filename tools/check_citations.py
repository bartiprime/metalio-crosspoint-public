#!/usr/bin/env python3
"""`file:line` citation checker for the docs -- exists, in range, and unambiguous.

The docs cite source as `path:line` (usually in backticks), with the house style
of a bare basename (`HalDisplay.h:42`) plus a prefix that names the tree
(`O:src/main.cpp:1877`, `V1:power_hw.cc:391`). A path-only resolver flags ~72%
of all citations "missing" purely because of that house style, so this tool:

  * takes a configurable set of roots (`--root`, repeatable; default: this
    firmware repo). Docs legitimately cite trees outside it (`M:/cpc`,
    `M:/_HermesWork/_diff/...`), and those roots are given on the command line,
    so a citation into a vendored tree is *checked*, not skipped;
  * understands the tree prefixes the docs use (`V1:power_hw.cc:391`,
    `O:src/main.cpp:1877`) through `--alias V1=<root>`, and looks for a
    prefixed citation inside that tree only -- a same-named file elsewhere
    cannot satisfy it;
  * resolves a citation by (1) the path as written against every root, then
    (2) by basename -- and reports a basename that matches more than one file
    as AMBIGUOUS rather than guessing;
  * with `--check-lines`, verifies the line number after the file resolves:
    beyond EOF is BROKEN (out-of-range), and a hit inside the last
    `--drift-threshold` lines is DRIFT (valid today, one edit from breaking);
  * with `--json`, emits one record per citation for a CI job to consume.

Bare `:420-426` citations (a continuation of the previous path) are counted but
not resolved: they cannot be checked without the surrounding sentence's subject,
and inventing one would be worse than reporting "relative".

Stdlib only, python 3. Exit 0 = no broken citation; exit 1 = at least one
MISSING or OUT-OF-RANGE (unless --no-fail). Ambiguity and drift are warnings.
"""

import argparse
import json
import os
import re
import sys

# Extensions that make "foo.ext:line" a code citation rather than prose.
CODE_EXTS = (
    "h", "hpp", "hh", "hxx", "c", "cc", "cpp", "cxx", "m", "mm", "ino", "S",
    "py", "sh", "bat", "ps1", "cmake", "mk", "json", "yaml", "yml", "toml",
    "md", "txt", "ld", "dts", "dtsi", "csv", "idf", "cfg", "ini", "conf",
)

# Explicit citation: [tree-prefix:]path:line[-line2]
CITE_RE = re.compile(
    r"(?<![\w./-])(?:[A-Za-z][A-Za-z0-9_]{0,3}:)?"
    r"((?:[A-Za-z]:[/\\])?[A-Za-z0-9_][A-Za-z0-9_./\\-]*?\.(%s))"
    r":(\d+)(?:-(\d+))?(?![\w.])" % "|".join(CODE_EXTS)
)
# A drive-letter path ("M:/_HermesWork/...") is absolute, not a tree prefix.
DRIVE_RE = re.compile(r"^[A-Za-z]:[/\\]")
# Bare continuation: ":420-426" with nothing before it on this citation.
BARE_LINE_RE = re.compile(r"(?<![\w:]):(\d+)(?:-(\d+))?(?![\w.])")

DEFAULT_DRIFT = 5
SKIP_DIRS = {
    ".git", ".hg", ".svn", "_scratch", "_deps", "node_modules", "build",
    "build-b0-tests", "build-hosttest", "_build", ".dart_tool", "baselines",
    "managed_components", "__pycache__", ".venv", "venv", "flashlogs",
}


class Citation:
    __slots__ = ("md_file", "md_line", "raw", "prefix", "path", "line", "end_line")

    def __init__(self, md_file, md_line, raw, prefix, path, line, end_line):
        self.md_file = md_file
        self.md_line = md_line
        self.raw = raw
        self.prefix = prefix
        self.path = path
        self.line = line
        self.end_line = end_line

    def as_dict(self):
        return {
            "doc": self.md_file,
            "doc_line": self.md_line,
            "citation": self.raw,
            "prefix": self.prefix,
            "path": self.path,
            "line": self.line,
            "end_line": self.end_line,
        }


def _clean_path(raw):
    """Normalize a cited path for lookup: unify slashes, drop a leading './'."""
    path = raw.replace("\\", "/").strip()
    while path.startswith("./"):
        path = path[2:]
    return path


def extract_citations(text, md_file="<text>"):
    """Pull every `path:line` citation out of one markdown file's text."""
    cites = []
    for idx, raw_line in enumerate(text.splitlines(), start=1):
        for m in CITE_RE.finditer(raw_line):
            whole = m.group(0)
            path = m.group(1)
            prefix = None
            # "M:/x/y.cc:12" -- the drive letter is part of the path, not a tree prefix.
            if not DRIVE_RE.match(whole) and ":" in whole:
                head = whole.split(":", 1)[0]
                if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,3}", head):
                    prefix = head
            cites.append(Citation(md_file, idx, whole, prefix,
                                  _clean_path(path), int(m.group(3)),
                                  int(m.group(4)) if m.group(4) else None))
    return cites


def count_relative(text):
    """Bare `:NNN` continuations -- counted, not resolved (see module doc)."""
    count = 0
    for raw_line in text.splitlines():
        # Remove explicit citations first so ":12" inside "foo.cc:12" is not
        # counted a second time.
        stripped = CITE_RE.sub(" ", raw_line)
        count += len(BARE_LINE_RE.findall(stripped))
    return count


def build_index(roots):
    """Map basename -> [absolute paths] for every file under each root."""
    by_base = {}
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                by_base.setdefault(name, []).append(os.path.join(dirpath, name))
    return by_base


def _under(candidate, root):
    """True when `candidate` lives inside `root` (absolute, slash-normalized)."""
    return os.path.abspath(candidate).replace("\\", "/").startswith(
        os.path.abspath(root).replace("\\", "/").rstrip("/") + "/")


def resolve_path(path, roots, index, within=None):
    """Return (status, abs_path|None, candidates). status: exact|basename|ambiguous|missing."""
    if DRIVE_RE.match(path):
        candidates = [path] if os.path.exists(path) else []
        if candidates:
            return "exact", candidates[0], candidates
    else:
        for root in roots:
            candidate = os.path.join(root, path)
            if os.path.exists(candidate) and os.path.isfile(candidate):
                return "exact", candidate, [candidate]
    base = os.path.basename(path)
    # Roots may overlap (a repo root inside a workspace root), so the same file
    # can be indexed twice; compare by absolute path, not by list position.
    candidates = list(dict.fromkeys(index.get(base, [])))
    if within is not None:
        # A tree prefix (`V1:`) names one tree: do not let a same-named file in
        # another tree satisfy (or break) the citation.
        candidates = [c for c in candidates if _under(c, within)]
    if len(candidates) == 1:
        return "basename", candidates[0], candidates
    if not candidates:
        return "missing", None, []
    if len(candidates) > 1:
        # Same relative path under two roots: prefer the one whose tail matches.
        tail_matches = [c for c in candidates if c.replace("\\", "/").endswith("/" + path)]
        if len(tail_matches) == 1:
            return "basename", tail_matches[0], candidates
        # Roots are ordered by priority: a copy of the same file in a later root
        # (an audit snapshot of the repo) is not ambiguity, it is duplication.
        for root in roots:
            subset = [c for c in candidates if _under(c, root)]
            if subset:
                if len(subset) == 1:
                    return "basename", subset[0], candidates
                break
        return "ambiguous", None, candidates
    return "missing", None, []


def file_line_count(path):
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    return data.count(b"\n") + (0 if data.endswith(b"\n") or not data else 1)


def check_citation(cite, roots, index, check_lines, drift, aliases=None):
    """Return a record dict: status in ok|missing|ambiguous|out-of-range|drift."""
    record = cite.as_dict()
    record["status"] = "ok"
    record["resolved"] = None
    record["detail"] = ""
    record["via"] = ""

    # A tree prefix (`V1:power_hw.cc`) names one tree; resolve inside it only.
    within = None
    if aliases and cite.prefix and cite.prefix in aliases:
        within = aliases[cite.prefix]
        record["via"] = "alias:" + cite.prefix

    how, resolved, candidates = resolve_path(cite.path, roots, index, within)
    if how == "missing":
        record["status"] = "missing"
        record["detail"] = "no file named %s under any root" % cite.path
        return record
    if how == "ambiguous":
        record["status"] = "ambiguous"
        record["detail"] = "%d files share that basename: %s" % (
            len(candidates), ", ".join(os.path.relpath(c, roots[0]) if c.startswith(roots[0]) else c
                                       for c in candidates[:4]))
        return record

    record["resolved"] = resolved
    record["via"] = how
    if not check_lines:
        return record

    lines = file_line_count(resolved)
    if lines is None:
        record["status"] = "missing"
        record["detail"] = "file exists but cannot be read: %s" % resolved
        return record
    cited = cite.end_line or cite.line
    if cite.line < 1 or cited > lines:
        record["status"] = "out-of-range"
        record["detail"] = "cited %d but the file has %d lines" % (cited, lines)
        return record
    if lines - cited <= drift:
        record["status"] = "drift"
        record["detail"] = "cited %d, file has %d lines (within %d of EOF)" % (cited, lines, drift)
    return record


def run(docs, roots, check_lines, drift, aliases=None):
    index = build_index(roots + ([aliases[a] for a in aliases] if aliases else []))
    all_records = []
    stats = {"docs": 0, "citations": 0, "relative": 0, "ok": 0,
             "missing": 0, "ambiguous": 0, "out-of-range": 0, "drift": 0}
    for doc in docs:
        try:
            with open(doc, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            print("cannot read %s: %s" % (doc, exc), file=sys.stderr)
            continue
        stats["docs"] += 1
        stats["relative"] += count_relative(text)
        for cite in extract_citations(text, doc):
            record = check_citation(cite, roots, index, check_lines, drift, aliases)
            stats["citations"] += 1
            stats[record["status"]] = stats.get(record["status"], 0) + 1
            all_records.append(record)
    return all_records, stats


def print_report(records, stats, docs, roots, check_lines, drift, verbose):
    print("=" * 78)
    print("CITATION CHECK — %d citation(s) in %d doc(s), roots: %s"
          % (stats["citations"], stats["docs"], ", ".join(roots)))
    print("=" * 78)
    if not check_lines:
        print("(path-only: run with --check-lines to verify line numbers)")
    else:
        print("(line numbers checked, drift threshold %d)" % drift)
    print()
    for record in records:
        if record["status"] == "ok" and not verbose:
            continue
        print("%-14s %s:%d  %s" % (record["status"].upper(), record["doc"],
                                  record["doc_line"], record["citation"]))
        if record["detail"]:
            print("               %s" % record["detail"])
    print()
    print("summary: %d ok, %d missing, %d ambiguous, %d out-of-range, %d drift, "
          "%d relative (unresolved, not counted)"
          % (stats["ok"], stats["missing"], stats["ambiguous"],
             stats["out-of-range"], stats["drift"], stats["relative"]))
    broken = stats["missing"] + stats["out-of-range"]
    print("broken (missing + out-of-range): %d" % broken)


def fixtures():
    """Self-test over synthetic docs + trees; proves each bucket is reachable."""
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory(prefix="cites-") as tmp:
        root = os.path.join(tmp, "repo")
        ext = os.path.join(tmp, "vendor")
        os.makedirs(os.path.join(root, "src"))
        os.makedirs(os.path.join(root, "other"))
        os.makedirs(os.path.join(ext, "deep", "dir"))
        with open(os.path.join(root, "src", "Real.h"), "w", encoding="utf-8") as fh:
            fh.write("\n".join("// line %d" % i for i in range(1, 101)) + "\n")
        with open(os.path.join(root, "src", "Twin.h"), "w", encoding="utf-8") as fh:
            fh.write("a\n")
        with open(os.path.join(root, "other", "Twin.h"), "w", encoding="utf-8") as fh:
            fh.write("a\n")
        with open(os.path.join(ext, "deep", "dir", "Twin.h"), "w", encoding="utf-8") as fh:
            fh.write("a\n")
        with open(os.path.join(ext, "deep", "dir", "External.cc"), "w", encoding="utf-8") as fh:
            fh.write("".join("// %d\n" % i for i in range(1, 31)))
        # The same file name in two different roots: first root wins, not ambiguous.
        with open(os.path.join(root, "other", "Copy.cc"), "w", encoding="utf-8") as fh:
            fh.write("".join("// %d\n" % i for i in range(1, 31)))
        with open(os.path.join(ext, "deep", "dir", "Copy.cc"), "w", encoding="utf-8") as fh:
            fh.write("".join("// %d\n" % i for i in range(1, 31)))
        with open(os.path.join(ext, "deep", "dir", "Only.h"), "w", encoding="utf-8") as fh:
            fh.write("".join("// %d\n" % i for i in range(1, 31)))

        cases = [
            ("resolvable basename", "Real.h:42", "ok"),
            ("line past EOF", "Real.h:173", "out-of-range"),
            ("line inside the drift band", "Real.h:99", "drift"),
            ("ambiguous basename (two files in one root)", "Twin.h:1", "ambiguous"),
            ("resolved in an external root", "External.cc:20", "ok"),
            ("same basename in two roots -> first root wins", "Copy.cc:5", "ok"),
            ("file that does not exist", "Gone.h:3", "missing"),
            ("missing file behind a tree prefix", "V1:Gone.cc:3", "missing"),
            ("tree prefix resolves inside its own tree", "V1:Only.h:5", "ok"),
            ("tree prefix does not fall back to another tree", "O:Only.h:5", "missing"),
            ("absolute path", os.path.join(root, "src", "Real.h") + ":10", "ok"),
        ]
        doc = os.path.join(tmp, "fixture.md")
        with open(doc, "w", encoding="utf-8") as fh:
            fh.write("Test doc.\n\n")
            for title, cite_text, _ in cases:
                fh.write("- %s: `%s`\n" % (title, cite_text))
            fh.write("- relative continuation: `:420-426`\n")

        records, stats = run([doc], [root, ext], check_lines=True, drift=DEFAULT_DRIFT,
                             aliases={"V1": ext, "O": root})
        records_by_raw = {}
        for record in records:
            records_by_raw.setdefault(record["citation"], record)
        seen = {record["citation"] for record in records}
        for title, cite_text, expected in cases:
            record = records_by_raw.get(cite_text)
            if record is None:
                print("FIXTURE FAIL  %s -- citation %r was not extracted" % (title, cite_text))
                ok = False
                continue
            if record["status"] != expected:
                print("FIXTURE FAIL  %s -- got %r, want %r (%s)"
                      % (title, record["status"], expected, record["detail"]))
                ok = False
        if len(seen) != len(cases):
            print("FIXTURE FAIL  %d distinct citations, want %d (duplicate extraction)"
                  % (len(seen), len(cases)))
            ok = False
        if stats["relative"] != 1:
            print("FIXTURE FAIL  relative count %d, want 1" % stats["relative"])
            ok = False
        print("fixtures: %d citations, %d ok / %d missing / %d ambiguous / %d out-of-range / %d drift"
              % (stats["citations"], stats["ok"], stats["missing"], stats["ambiguous"],
                 stats["out-of-range"], stats["drift"]))
    print("FIXTURES %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check `file:line` citations in markdown")
    parser.add_argument("docs", nargs="*", help="markdown files to check")
    parser.add_argument("-r", "--root", action="append", default=None,
                        help="tree to resolve citations against (repeatable). "
                             "Default: the firmware repo this tool lives in.")
    parser.add_argument("--json", action="store_true", help="emit records as JSON")
    parser.add_argument("--check-lines", action="store_true",
                        help="verify the cited line number (EOF + drift)")
    parser.add_argument("--drift-threshold", type=int, default=DEFAULT_DRIFT,
                        help="lines this close to EOF are reported as drift (default %d)"
                             % DEFAULT_DRIFT)
    parser.add_argument("--no-fail", action="store_true",
                        help="always exit 0 (report only)")
    parser.add_argument("-v", "--verbose", action="store_true", help="show ok citations too")
    parser.add_argument("--alias", action="append", default=None, metavar="PREFIX=ROOT",
                        help="map a tree prefix used by the docs (V1:, O:) to a root, "
                             "so `V1:foo.cc:12` is looked for in that tree only")
    parser.add_argument("--fixtures", action="store_true", help="run the self-test and exit")
    args = parser.parse_args(argv)

    if args.fixtures:
        return fixtures()

    docs = args.docs
    if not docs:
        print("no docs given (pass one or more markdown files, or --fixtures)", file=sys.stderr)
        return 2

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    roots = args.root or [repo_root]
    roots = [os.path.abspath(os.path.expanduser(r)) for r in roots]
    for root in roots:
        if not os.path.isdir(root):
            print("warning: root does not exist: %s" % root, file=sys.stderr)

    aliases = {}
    for spec in args.alias or []:
        prefix, _, alias_root = spec.partition("=")
        if not _:
            print("bad --alias %r (want PREFIX=ROOT)" % spec, file=sys.stderr)
            return 2
        aliases[prefix] = os.path.abspath(os.path.expanduser(alias_root))
        if not os.path.isdir(aliases[prefix]):
            print("warning: alias root does not exist: %s" % aliases[prefix], file=sys.stderr)

    records, stats = run(docs, roots, args.check_lines, args.drift_threshold, aliases)

    if args.json:
        print(json.dumps({
            "roots": roots,
            "docs": docs,
            "check_lines": args.check_lines,
            "drift_threshold": args.drift_threshold,
            "stats": stats,
            "records": records,
        }, indent=2))
    else:
        print_report(records, stats, docs, roots, args.check_lines,
                     args.drift_threshold, args.verbose)

    broken = stats["missing"] + stats["out-of-range"]
    if args.no_fail:
        return 0
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
