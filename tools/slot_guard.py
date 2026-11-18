#!/usr/bin/env python3
"""SLOT-GUARD — the shared UI theme slot may only be rebuilt from a fresh target.

The class of defect this stops (three instances so far: TitledHeaderBand.h,
UiTabListActivity.cpp, and the still-open UiAppHost.cpp resetUi()):
`applySharedUiTheme()` / `refreshSharedUiThemeTokens()` take a
`GfxRendererTarget`, and the target is what binds the uiScale font faces. A
caller that passes a CACHED target (a member, a local built once at construction)
re-publishes a token set derived from whatever font binding that target happened
to have when it was made. The screen then paints with another screen's faces --
the "band is right, the list is not" / "tab label face" split.

The rule: the argument must be a `makeUiTarget(...)` expression, evaluated at the
call. Two documented escapes:

  1. an inline comment on the same line, or on any line of the call, containing
        SLOT-GUARD-ALLOW: <reason>
     for a legitimate caller that really does mean to re-publish from a target it
     owns (e.g. a target it created from makeUiTarget() two lines up and has not
     mutated). The reason is required; an empty one is a violation.
  2. an entry in tools/slot_guard_allowlist.txt, `path::function  reason`, for a
     known-open instance that is scheduled to be fixed and must not be silently
     forgotten. Each entry is printed as KNOWN at every run.

Exit code 0 = clean, 1 = violation. Run with no arguments from anywhere.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = [os.path.join(ROOT, d) for d in ("src", "lib", "freeink-sdk")]
ALLOWLIST = os.path.join(ROOT, "tools", "slot_guard_allowlist.txt")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".cxx")

CALLS = ("applySharedUiTheme", "refreshSharedUiThemeTokens")


def strip_comments_preserving_lines(text):
    """Blank out // and /* */ comments but keep every newline, so line numbers
    and the SLOT-GUARD-ALLOW token can still be recovered from the raw text."""
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
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def split_args(argtext):
    """Top-level comma split, paren/bracket/angle aware enough for these calls."""
    args = []
    depth = 0
    cur = []
    for ch in argtext:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        args.append("".join(cur).strip())
    return args


def normalise(expr):
    return re.sub(r"\s+", " ", expr.strip())


def load_allowlist():
    entries = {}
    if not os.path.exists(ALLOWLIST):
        return entries
    with open(ALLOWLIST, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(None, 2)
            if len(parts) < 2 or parts[0] not in ("open", "legit"):
                raise SystemExit("slot_guard_allowlist.txt: bad line %r "
                                 "(want `<open|legit> <path>::<function> <reason>`)" % line)
            entries[parts[1]] = (parts[0], parts[2] if len(parts) > 2 else "")
    return entries


def enclosing_function(stripped, offset):
    """Best-effort: the last identifier-looking(token) { before offset whose
    brace block still contains offset. Falls back to '<file scope>'."""
    best = "<file scope>"
    for m in re.finditer(r"([A-Za-z_~][\w:~<>*&\s]*?)\s*\(([^;{}]*)\)\s*(?:const\s*)?\{", stripped):
        if m.start() > offset:
            break
        # does this block contain offset?
        depth = 1
        j = m.end()
        while j < len(stripped) and depth:
            if stripped[j] == "{":
                depth += 1
            elif stripped[j] == "}":
                depth -= 1
            j += 1
        if depth == 0 and m.end() <= offset < j:
            name = m.group(1).strip().split()[-1] if m.group(1).strip() else "?"
            best = name.split("::")[-1]
    return best


def find_argument_list(stripped, open_paren):
    depth = 1
    i = open_paren + 1
    while i < len(stripped):
        if stripped[i] == "(":
            depth += 1
        elif stripped[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def scan():
    allow = load_allowlist()
    violations = []
    known = []
    allowed_inline = 0
    checked = 0

    for base in SCAN_DIRS:
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    raw = fh.read()
                stripped = strip_comments_preserving_lines(raw)
                raw_lines = raw.splitlines()

                for call in CALLS:
                    for m in re.finditer(r"\b" + call + r"\s*\(", stripped):
                        # a definition/declaration, not a call: `... refreshSharedUiThemeTokens(const ...&`
                        open_paren = stripped.index("(", m.start())
                        close_paren = find_argument_list(stripped, open_paren)
                        if close_paren < 0:
                            continue
                        argtext = stripped[open_paren + 1:close_paren]
                        args = split_args(argtext)
                        if not args:
                            continue
                        # Definition site: the "argument" is a declaration.
                        if any(re.search(r"\b(const|auto|std::|freeink::)\b", a) for a in args):
                            continue
                        checked += 1
                        # applySharedUiTheme(app, target): the target is last.
                        target_arg = args[-1]
                        if normalise(target_arg).startswith("makeUiTarget("):
                            continue

                        start_line = stripped.count("\n", 0, m.start())
                        end_line = stripped.count("\n", 0, close_paren)
                        window = "\n".join(raw_lines[max(0, start_line - 3):end_line + 1])
                        if "SLOT-GUARD-ALLOW:" in window:
                            reason = window.split("SLOT-GUARD-ALLOW:", 1)[1].strip()
                            if reason:
                                allowed_inline += 1
                                continue
                            violations.append((rel, start_line + 1, call, target_arg, "",
                                               "SLOT-GUARD-ALLOW marker with no reason"))
                            continue

                        fn = enclosing_function(stripped, m.start())
                        # The definitions of these two helpers live in
                        # UiAppHelpers.h and forward to each other; a call inside
                        # its own definition body is not a call site.
                        if fn in CALLS:
                            continue
                        key = "%s::%s" % (rel, fn)
                        if key in allow:
                            cls, reason = allow[key]
                            known.append((cls, key, start_line + 1, target_arg, reason))
                            continue
                        violations.append((rel, start_line + 1, call, target_arg, key, ""))

    for cls, key, line, arg, reason in known:
        tag = "KNOWN-OPEN" if cls == "open" else "LEGIT"
        print("%-10s %s:%d  %s   (%s)" % (tag, key, line, normalise(arg), reason))

    if not violations:
        print("SLOT-GUARD ok: %d call(s) checked, %d inline allowance(s), %d allowlisted"
              % (checked, allowed_inline, len(known)))
        return 0

    for rel, line, call, arg, key, note in violations:
        print("VIOLATION  %s:%d  %s(... %s)" % (rel, line, call, normalise(arg)))
        if key:
            print("           enclosing function key: %s" % key)
            print("           this is a NEW instance of the shared-slot class, not an allowlisted one")
        if note:
            print("           %s" % note)
    print()
    print("SLOT-GUARD FAILED: %d violation(s) of %d call(s) checked." % (len(violations), checked))
    print("Every argument to applySharedUiTheme()/refreshSharedUiThemeTokens() must be a")
    print("makeUiTarget(...) expression evaluated at the call, or carry an inline")
    print("`SLOT-GUARD-ALLOW: <reason>` comment, or be listed in tools/slot_guard_allowlist.txt.")
    return 1


if __name__ == "__main__":
    sys.exit(scan())
