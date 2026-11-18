#!/bin/sh
# Build and run the host-side theme checks. NO ESP BUILD: this compiles two host
# translation units against the theme headers only (g++ from MSYS / any C++17),
# and never touches the firmware, the panel or the toolchain.
#
#   sh tools/theme_tokens/run.sh            # checks + token table on stdout
#   sh tools/theme_tokens/run.sh > t.md     # the review table, generated
#
# WHAT IT PROVES
#   host_table.cpp   prints every ThemeMetrics field, per theme, straight out of
#                    the compiled constexpr tables + its MetricScale class.
#   host_verify.cpp  asserts the design language and the home geometry, so a
#                    token that drifts from the language fails here and not on
#                    the glass.
#   check_theme_registration.py  asserts the enum / picker-label / persisted-value
#                    alignment, which is what a wrong append would break.
#   check_theme_syntax.sh  type-checks the theme .cpp itself against the real
#                    BaseTheme.h (see that script: it is what makes the first
#                    flash the first LINK, not the first compile).
#
# WHAT IT CANNOT PROVE: anything about ink on e-paper. Refresh behaviour, ghosting
# and stroke legibility at 297 PPI are glass judgements.
set -e
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"

CXX=${CXX:-g++}
# Relative include paths on purpose: this host runs a native g++ under MSYS,
# where an /m/... argument is not translated for a native program.
INC="-I tools/theme_tokens/stubs -I src"

"$CXX" -std=c++17 $INC -O1 -o tools/theme_tokens/table.exe tools/theme_tokens/host_table.cpp
"$CXX" -std=c++17 $INC -O1 -o tools/theme_tokens/verify.exe tools/theme_tokens/host_verify.cpp

./tools/theme_tokens/verify.exe
echo
./tools/theme_tokens/table.exe
echo
python3 tools/theme_tokens/check_theme_registration.py .
echo
sh tools/theme_tokens/check_theme_syntax.sh
