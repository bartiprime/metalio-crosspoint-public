// r163 D3 -- grey admission by CONTENT ROLE.
//
// lib/GfxRenderer/ContentRole.h is the whole table, and it is header-only, so the
// admission rule the panel honours is asserted here rather than inferred from glass.
//
// The two properties that matter, and why:
//
//   1. AN ICON IS NEVER GREY. Not "an icon drawn this way", not "an icon on a FAST
//      paint": the ROLE refuses intermediate levels, so a status glyph cannot start
//      ghosting because someone re-sourced it (a 4 bpp sprite, a scaled blit).
//
//   2. EVERYTHING ELSE -- INCLUDING UNTAGGED -- ADMITS GREY. `Untagged` is the
//      legacy mapping for every call site this batch did not re-tag, which is what
//      makes the change invisible for content nobody classified: admission is the
//      paint's own decision there, exactly as before.
//
// Deliberately NOT asserted here: which refresh mode a frame takes. That is
// RefreshPolicy's decision and this rule must not be able to move it.

#include <gtest/gtest.h>

#include <GfxRenderer/ContentRole.h>

namespace {

using gfxcontent::Role;

TEST(GreyRole, EveryDeclaredRoleHasAName) {
  // The roles this device can actually TAG: the vendor's Cover and Illustration, our
  // Icon (the one that denies) and the Untagged default. Monochrome/FastMonochrome/
  // Shelf/Text are not declared: no call site ever produced them.
  const Role roles[] = {Role::Untagged, Role::Cover, Role::Illustration, Role::Icon};
  for (const Role r : roles) {
    EXPECT_STRNE(gfxcontent::nameOf(r), "?") << "every role must have a name for the debug surface";
  }
}

TEST(GreyRole, OnlyIconDeniesGrey) {
  EXPECT_FALSE(gfxcontent::admitsGreyscale(Role::Icon));
  EXPECT_TRUE(gfxcontent::admitsGreyscale(Role::Untagged));
  EXPECT_TRUE(gfxcontent::admitsGreyscale(Role::Cover));
  EXPECT_TRUE(gfxcontent::admitsGreyscale(Role::Illustration));
}

TEST(GreyRole, UntaggedIsTheLegacyDefaultSoNothingChangesSilently) {
  // A call site nobody tagged must behave exactly as it did before this batch.
  EXPECT_EQ(static_cast<int>(Role::Untagged), 0) << "0 must stay the default value";
  EXPECT_TRUE(gfxcontent::admitsGreyscale(Role::Untagged));
}

TEST(GreyRole, TheTableIsConstexprSoTheAdmissionCostsNothingAtRuntime) {
  static_assert(gfxcontent::admitsGreyscale(Role::Cover));
  static_assert(!gfxcontent::admitsGreyscale(Role::Icon));
  SUCCEED();
}

}  // namespace
