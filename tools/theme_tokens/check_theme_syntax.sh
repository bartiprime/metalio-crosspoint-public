#!/bin/sh
# Host SYNTAX/TYPE check for a theme translation unit. NO ESP BUILD.
#
#   sh tools/theme_tokens/check_theme_syntax.sh
#   sh tools/theme_tokens/check_theme_syntax.sh src/components/themes/pickyos/PickyOSTheme.cpp
#
# It compiles the .cpp with `-fsyntax-only` against the REAL BaseTheme.h and the
# REAL fontIds.h / icon headers, and against minimal host stubs for the platform
# headers (GfxRenderer, UITheme, HalStorage, HalClock, ...) under synth/. That is
# what turns "the first flash is the first compile" into "the first flash is the
# first *link*": virtual signatures, override correctness, member names on the
# theme base and every renderer call are all checked here.
#
# THE STUBS ARE NOT THE FIRMWARE. Every renderer signature in synth/GfxRenderer.h
# is copied verbatim from lib/GfxRenderer/GfxRenderer.h; if the real header gains
# a parameter, this check does not notice. Re-copy when the renderer changes.
set -e
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
tus=${*:-src/components/themes/pickyos/PickyOSTheme.cpp}
CXX=${CXX:-g++}

"$CXX" -std=c++17 -fsyntax-only -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I tools/theme_tokens/synth \
  -I tools/theme_tokens/stubs \
  -I src -I lib/EpdFont -I freeink-sdk/libs/assets/Icons/include \
  $tus
echo "syntax ok: $tus"
