#pragma once

// Power-off handshake between the confirmation dialog and main.cpp's shutdown path.
//
// The dialog paints itself as "Powering off..." through the NORMAL render path and only then
// raises the request; loop() sees it and runs the shutdown WITHOUT painting, because the frame is
// already on the glass. Defined in main.cpp (file scope there), declared here so the dialog can
// reach them without including the sketch.
void metalioRequestPowerOff();
bool metalioPowerOffRequested();
// The dialog closed (either answer). Clears the hold gate so the classifier can classify fresh
// presses again -- otherwise a released key would re-fire the dialog every 4 s.
void metalioPowerConfirmClosed(bool closed);

// PAINT-GAP TIMING (r100.4). The press that summons the dialog is classified by main.cpp's
// loop(); the frame it paints is produced by the dialog, on the render task. Two different
// places, one gesture -- so the two stamps live here, on one monotonic clock (millis()):
//
//   * noteDownEdge      -- the tick that first saw the power key DOWN for the press under
//                          classification (clears any previous decision);
//   * noteDialogDecision -- the tick the classifier decided DIALOG and pushed this dialog.
//
// The dialog logs both at its first paint that actually reaches the glass:
//   [PWR] dialog first paint: decision at 3003 ms, painted at 30xx ms after DOWN (gap NN ms)
// That line answers "the 3 s hold feels like 5 s" without counting seconds by hand: if the gap
// is large, the missing time was the paint, not the threshold.
void metalioPowerNoteDownEdge(unsigned long ms);
void metalioPowerNoteDialogDecision(unsigned long ms);
unsigned long metalioPowerDownEdgeMs();        // 0 = no press recorded since boot
unsigned long metalioPowerDialogDecisionMs();  // 0 = this press has not decided DIALOG yet


class GfxRenderer;

// The final frame's label. One literal for both paths that paint it: the awake dialog and the
// standby auto-off path.
inline constexpr const char* kPowerOffFrameText = "Powered off";

// Paints the powered-off frame (a dialog box with the status text) as a FULL drive and blocks
// until it is on the glass. Returns the drive duration in ms so callers can journal it.
unsigned long metalioPaintPoweredOffFrame(GfxRenderer& renderer);

// r101.21 PWR4: which frame that paint actually used -- "image", "text", "dark", "light", "blank"
// ("none" before the first call). A POWER_OFF_CUSTOM whose image could not be read reports "text":
// it fell through to the generic dialog. Read by main.cpp's standby auto-off journal line so a
// substituted frame is visible on the card.
const char* metalioPowerOffFrameLabel();

// r101.10 PWR3: TRUE only if the confirmation dialog verified a panel drive AFTER its blocking
// paint wait -- i.e. the frame it composed is on the glass. main.cpp reads this immediately before
// display.deepSleep() and paints the frame itself when it is false, so a parked unpainted panel is
// impossible instead of merely unlikely.
bool metalioPowerOffFrameDriven();
