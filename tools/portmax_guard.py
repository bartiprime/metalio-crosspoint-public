#!/usr/bin/env python3
"""PORTMAX-LOOP — no unbounded take inside a loop outside the render task.

The r101.10 freeze had exactly one shape: `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`
inside a retry loop. A loop multiplies a blocking take by its iteration count, so
a rendezvous that is never answered does not cost a timeout -- it costs the UI.
The one legitimate loop is the render task's own `for (;;)`, where blocking until
there is work is the point.

Rule: a `portMAX_DELAY` inside a for/while body is a violation unless the
enclosing function is the render task. `tools/portmax_guard_allowlist.txt` names
the exceptions (function + reason); an entry that no longer matches is reported
as STALE so the allowlist cannot rot into a blanket exemption.

Exit code 0 = clean, 1 = violation.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = [os.path.join(ROOT, d) for d in ("src", "lib")]
ALLOWLIST = os.path.join(ROOT, "tools", "portmax_guard_allowlist.txt")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".cxx", ".c")


def strip_comments_and_strings(text):
    """Keep newlines so offsets and line numbers survive; blank comments and
    string/char literals so a needle in prose or in a log format cannot match."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif text.startswith("/*", i):
            out.append("  ")
            i += 2
            while i < n and not text.startswith("*/", i):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
        elif text[i] in "\"'":
            quote = text[i]
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append(" ")
                i += 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def match_brace(text, open_pos):
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


KEYWORDS = {"for", "while", "if", "switch", "catch", "do", "else", "return", "sizeof"}


def enclosing_function(text, offset):
    best = ("<file scope>", 0, len(text))
    for m in re.finditer(r"([A-Za-z_~][\w:~<>*&\s]*?)\s*\(([^;{}]*)\)\s*(?:const\s*)?\{", text):
        if m.start() > offset:
            break
        raw = m.group(1).strip()
        name = raw.split()[-1] if raw.split() else "?"
        if name.split("::")[-1] in KEYWORDS:
            continue
        close = match_brace(text, m.end() - 1)
        if close > offset > m.start():
            best = (name.split("::")[-1], m.start(), close)
    return best[0]


def loop_bodies(stripped):
    """(loop header offset, body start, body end) for every for/while with a brace
    body. A braceless single-statement loop body is taken to its semicolon."""
    out = []
    for m in re.finditer(r"\b(for|while)\s*\(", stripped):
        open_paren = stripped.index("(", m.start())
        depth = 1
        i = open_paren + 1
        while i < len(stripped) and depth:
            if stripped[i] == "(":
                depth += 1
            elif stripped[i] == ")":
                depth -= 1
            i += 1
        if depth:
            continue
        j = i
        while j < len(stripped) and stripped[j] in " \t\r\n":
            j += 1
        if j < len(stripped) and stripped[j] == "{":
            end = match_brace(stripped, j)
            if end > 0:
                out.append((m.start(), j, end))
        else:
            semi = stripped.find(";", j)
            if semi > 0:
                out.append((m.start(), j, semi))
    return out


def load_allowlist():
    entries = {}
    if not os.path.exists(ALLOWLIST):
        return entries
    with open(ALLOWLIST, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(None, 1)
            entries[parts[0]] = parts[1] if len(parts) > 1 else ""
    return entries


def main():
    allow = load_allowlist()
    seen = set()
    violations = []
    checked_files = 0
    checked_loops = 0

    for base in SCAN_DIRS:
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                stripped = strip_comments_and_strings(text)
                if "portMAX_DELAY" not in stripped:
                    continue
                checked_files += 1
                bodies = loop_bodies(stripped)
                checked_loops += len(bodies)
                for m in re.finditer(r"\bportMAX_DELAY\b", stripped):
                    for header, start, end in bodies:
                        if start <= m.start() <= end:
                            fn = enclosing_function(stripped, m.start())
                            key = "%s::%s" % (rel, fn)
                            line = stripped.count("\n", 0, m.start()) + 1
                            if "render" in fn.lower() and "task" in fn.lower():
                                seen.add(key)
                                break
                            if key in allow:
                                seen.add(key)
                                break
                            violations.append((rel, line, fn))
                            break

    for key, reason in allow.items():
        if key not in seen:
            print("STALE  %s   (allowlisted but no unbounded take found inside a loop any more: %s)"
                  % (key, reason or "no reason given"))

    if not violations:
        print("PORTMAX-LOOP ok: %d file(s) with a portMAX_DELAY, %d loop body/bodies scanned"
              % (checked_files, checked_loops))
        return 0

    for rel, line, fn in violations:
        print("VIOLATION  %s:%d  portMAX_DELAY inside a loop in %s()" % (rel, line, fn))
    print()
    print("PORTMAX-LOOP FAILED: %d unbounded take(s) inside a loop." % len(violations))
    print("A retry loop multiplies a blocking take: bound it (pdMS_TO_TICKS) or move the")
    print("blocking wait out of the loop. The render task's own loop is the only exception.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
