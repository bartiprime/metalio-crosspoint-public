#pragma once

#include <string>

// Convert an HTML fragment to plain text for the dictionary's plain-text
// fallback. Styling is ignored: block elements become line breaks and HTML
// entities are decoded. A heading becomes a label line of its own: the
// headingMarker byte is written at the start of that line, and the definition
// view draws a line starting with it in bold.
// Reuses the input buffer: pass an owned string by move to avoid a copy, and
// the plain text is compacted into it in place (it is never longer than the
// markup it came from).
std::string htmlToPlainText(std::string html, char headingMarker);
