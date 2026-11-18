#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// r163 D3 -- CONTENT ROLE: the admission tag for the grey lane.
//
// Separate from GfxRenderer.h so the admission table can be compiled and tested on
// the host (test/grey_role) without Arduino, a panel or a framebuffer. GfxRenderer
// aliases `Role` as `GfxRenderer::ContentRole`; nothing else should need this header.
//
// WHAT THIS IS NOT. It is an ADMISSION rule, not a lane rule. It decides whether a
// pixel may carry an intermediate level; it does not choose a refresh mode, does not
// touch a waveform and is not consulted by RefreshPolicy. A role can only ever DENY
// grey -- never grant it -- so tagging an element can make the frame MORE binary and
// never more grey. That asymmetry is deliberate: it means no tag in this batch can
// make an element newly able to ghost.
//
// THE VENDOR'S SET, AND WHAT WE KEPT. The vendor names five content classes:
// Monochrome, FastMonochrome, Shelf, Cover, Illustration, plus our Icon. Only the roles
// this device actually TAGS are declared here -- Untagged, Cover, Illustration, Icon. The
// other three names (Monochrome, FastMonochrome, Shelf) and our own Text role were never
// produced by any call site, so they are not kept as placeholders: an enum value nothing
// constructs is a slot a future tag would have to guess the meaning of. A new tag picks a
// role because it needs one, and adds it here with its call site in the same change.
//
// Only visible content opts into grey; an `Icon` stays binary whatever its source format
// is (a 4 bpp sprite, a scaled blit, an antialiased primitive).
// ---------------------------------------------------------------------------

namespace gfxcontent {

enum class Role : uint8_t {
  // The legacy default: the paint's own intent decides. Every call site that is not
  // explicitly tagged lands here, so an untagged element behaves exactly as before.
  Untagged = 0,
  Cover,         // a book cover image (the one visible-content tag on the themes' path)
  Illustration,  // any other visible artwork (a logo, a plate, the boot/sleep logo)
  Icon,          // a status or UI glyph -- NEVER grey
};

constexpr bool admitsGreyscale(const Role role) { return role != Role::Icon; }

inline const char* nameOf(const Role role) {
  switch (role) {
    case Role::Untagged:
      return "untagged";
    case Role::Cover:
      return "cover";
    case Role::Illustration:
      return "illustration";
    case Role::Icon:
      return "icon";
  }
  return "?";
}

}  // namespace gfxcontent
