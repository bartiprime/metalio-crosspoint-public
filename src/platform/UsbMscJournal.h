#pragma once

#include <stdbool.h>

// Journal for the USB-MSC drive-mode session (scope C4).
//
// WHY THIS IS NOT JUST LOG_INF()
//   The drive-mode session hands the SD card to the USB host and never takes it
//   back live: the volume stays detached until the reboot. Logical logging
//   therefore has two windows, and they need two mechanisms:
//
//   * ENTER happens while the filesystem is still mounted, so enter() appends
//     the line straight to /.crosspoint/debug.log -- before the hand-over, which
//     is the only moment it can be written at all.
//   * EXIT happens after the hand-over, when there is no volume to append to.
//     exit() captures the line in RTC_NOINIT memory instead; flushDeferred()
//     appends it to the same file on the next boot. A cold boot is the exit, so
//     "buffered until reboot" is exactly one boot of delay, never lost.
namespace usbmscjournal {

// Append one already-formatted line (with the "[ms] [LVL] [TAG] " prefix) to
// /.crosspoint/debug.log. Returns false once the volume is detached.
bool writeLine(const char* line);

// Capture one already-formatted line in RTC_NOINIT memory for the next boot.
void deferLine(const char* line);

// Append the deferred line, if any, and clear it. Call once at boot, after
// Storage.begin() and before anything else wants the log.
bool flushDeferred();

}  // namespace usbmscjournal
