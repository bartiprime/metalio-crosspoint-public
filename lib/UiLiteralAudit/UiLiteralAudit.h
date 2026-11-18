#pragma once

#include <cstddef>

// AUDIT2 -- read-only report of integer literals used where a UI metric is expected.
//
// The companion to UITheme::auditUiScale(). That audit walks kMetricFields[] and proves every
// *metric* was scaled; it cannot see a screen that never consults a metric at all and writes a
// pixel number straight into the draw call. `U-2` (tab band = literal 50 px) and `U-3` (toggle =
// literal 28 px) were both invisible to it for that reason and were found on glass instead.
//
// A literal lives in the source, not in the running image, so the list is produced at BUILD time
// (scripts/gen_ui_literals.py -> UiLiteralAudit.generated.h, wired up in the component's
// CMakeLists) and compiled in here. The boot path writes it to /.crosspoint/debug.log beside the
// UISCALE line, so the class is readable from a logtail with no host-side step.
//
// Nothing in this module draws, decides or scales anything: it is a table and two accessors.
namespace uiLiteralAudit {

// Number of findings in the compiled-in table.
int count();

// Number of distinct source files those findings live in.
int fileCount();

// One finding, "<path relative to src/>:<line> <name>=<literal>", or "" when out of range.
const char* entry(int index);

}  // namespace uiLiteralAudit
