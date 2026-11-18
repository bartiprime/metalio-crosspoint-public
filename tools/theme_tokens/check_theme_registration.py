#!/usr/bin/env python3
"""Registration meta-check for the UI theme list.

`uiTheme` is persisted as an INTEGER (CrossPointSettings::UI_THEME), and the
settings row is a positional list (SettingsList.h homeThemeValues()), where the
vector index IS the persisted value. So three things must stay in lockstep, and
a mistake in any of them silently re-themes somebody's device:

  1. the enum values,
  2. the order of homeThemeValues()'s label array,
  3. a label string, a `case` in UITheme::setTheme(), and a PSRAM-degraded
     relabel that touches ONLY the cover-grid slot.

This script asserts all of that from the source. It does not compile anything,
so it runs anywhere python3 does. Exit code is the verdict.

Usage: python3 tools/theme_tokens/check_theme_registration.py [repo_root]
"""
import re
import sys
from pathlib import Path

# ENUM NAME -> the StrId the picker must show at that index.
ENUM_TO_STR = {
    "CLASSIC": "STR_THEME_CLASSIC",
    "LYRA": "STR_THEME_LYRA",
    "LYRA_3_COVERS": "STR_THEME_LYRA_EXTENDED",
    "ROUNDEDRAFF": "STR_THEME_ROUNDEDRAFF",
    "COVER_GRID": "STR_THEME_COVER_GRID",
    "ANDRO": "STR_THEME_ANDRO",
    "PICKYOS": "STR_THEME_PICKYOS",
}

failures = []


def check(ok, what, detail=""):
    print(("ok    " if ok else "FAIL  ") + what + (("  <- " + detail) if detail and not ok else ""))
    if not ok:
        failures.append(what)


def main(root):
    root = Path(root)

    settings = (root / "src/CrossPointSettings.h").read_text(encoding="utf-8")
    enum_body = re.search(r"enum UI_THEME\s*\{([^}]*)\}", settings)
    check(enum_body is not None, "UI_THEME enum found")
    if not enum_body:
        return 1
    enum = {}
    order = []
    for part in enum_body.group(1).split(","):
        part = part.strip()
        if not part:
            continue
        name, value = [p.strip() for p in part.split("=")]
        enum[name] = int(value)
        order.append(name)

    # 1. Values are dense 0..N and appended, never inserted.
    check(sorted(enum.values()) == list(range(len(enum))), "enum values are dense from 0", str(enum))
    check(enum.get("ANDRO") == 5 and enum.get("PICKYOS") == 6,
          "Andro keeps 5 and PickyOS is appended at 6", str(enum))
    check(all(ENUM_TO_STR.get(n) for n in enum), "every enum value has a label mapping", str(list(enum)))

    # 2. The picker's label array is index-aligned with the enum.
    slist = (root / "src/SettingsList.h").read_text(encoding="utf-8")
    values = re.search(r"static constexpr StrId VALUES\[\]\s*=\s*\{(.*?)\};", slist, re.S)
    check(values is not None, "homeThemeValues() label array found")
    if not values:
        return 1
    ids = re.findall(r"StrId::(STR_[A-Z0-9_]+)", values.group(1))
    check(len(ids) == len(enum), "label array length == enum size", f"{len(ids)} vs {len(enum)}")
    for i, id_ in enumerate(ids):
        want = ENUM_TO_STR.get(order[i] if i < len(order) else "")
        check(id_ == want, f"index {i} ({order[i] if i < len(order) else '?'}) -> {id_}", f"want {want}")

    # 3. The PSRAM-degraded path relabels the cover-grid SLOT and nothing else:
    #    it must not shift the vector (which would change every stored value).
    check(re.search(r"values\[CrossPointSettings::COVER_GRID\]\s*=", slist) is not None,
          "degraded path relabels the COVER_GRID slot in place")
    check("std::size(VALUES) - 1" not in slist,
          "degraded path does NOT truncate the list (no index shift)")

    # 4. Every enum value has a case in UITheme::setTheme().
    uitheme = (root / "src/components/UITheme.cpp").read_text(encoding="utf-8")
    for name in enum:
        check(f"CrossPointSettings::UI_THEME::{name}" in uitheme or
              f"CrossPointSettings::{name}" in uitheme, f"setTheme() handles {name}")

    # 5. Every label has an English string.
    english = (root / "lib/I18n/translations/english.yaml").read_text(encoding="utf-8")
    for name in enum:
        key = ENUM_TO_STR[name]
        check(re.search(rf"^{key}\s*:", english, re.M) is not None, f"english.yaml has {key}")

    print()
    print("FAILED (%d)" % len(failures) if failures else "ALL CHECKS PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
