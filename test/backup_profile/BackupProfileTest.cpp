#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "BackupProfile.h"
#include "BackupSha256.h"
// The bound is spelled out in both modules; this test pins them together.
#include "src/util/ReadingStats.h"

namespace {

std::string payloadJson() {
  return std::string(
      "{\"settings\":{\"uiTheme\":4,\"fontSize\":14,\"language\":\"EN\","
      "\"sdFontFamilyName\":\"Literata\"},\"reading\":{\"recent\":{\"books\":[]}},"
      "\"credentials\":{\"wifi\":{\"credentials\":[]},\"koreader\":{\"username\":\"\"}}}");
}

JsonDocument makePayload() {
  JsonDocument doc;
  deserializeJson(doc, payloadJson());
  return doc;
}

JsonDocument makeEnvelope(bool tamper = false) {
  JsonDocument payload = makePayload();
  std::string text = backup::buildEnvelope(payload.as<JsonVariantConst>(), "r102", "2026-11-09T20:00:00+08:00");
  EXPECT_FALSE(text.empty());
  if (tamper) {
    // One byte inside the payload: the sha cannot agree any more.
    const std::size_t at = text.find("\"uiTheme\":4");
    if (at == std::string::npos) return JsonDocument();
    text[at + 10] = '9';
  }
  JsonDocument doc;
  deserializeJson(doc, text);
  return doc;
}

}  // namespace

TEST(BackupSha256, NistVectors) {
  EXPECT_EQ(backup::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(backup::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(backup::sha256Hex("The quick brown fox jumps over the lazy dog"),
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  // A length that crosses a block boundary and the 56-byte padding split.
  std::string block(64, 'a');
  EXPECT_EQ(backup::sha256Hex(block), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(BackupEnvelope, RoundTripValid) {
  JsonDocument doc = makeEnvelope();
  EXPECT_EQ(backup::parseEnvelope(doc), backup::Reject::None);
  EXPECT_EQ(doc["crosspoint_backup"]["schema"].as<int>(), backup::kSchema);
  EXPECT_STREQ(doc["crosspoint_backup"]["app"].as<const char*>(), "r102");
  EXPECT_STREQ(doc["crosspoint_backup"]["device"].as<const char*>(), backup::kDevice);
  EXPECT_STREQ(doc["crosspoint_backup"]["kind"].as<const char*>(), backup::kKindFullProfile);
  EXPECT_TRUE(doc["payload"]["settings"].is<JsonObject>());
}

TEST(BackupEnvelope, ShaSurvivesQuotesNewlinesAndUnicode) {
  JsonDocument payload;
  const char* raw =
      "{\"settings\":{\"dictionaryName\":\"a\\\"b\"},"
      "\"reading\":{\"recent\":{\"books\":[{\"title\":\"cafÃ© \u2014 line\n2\"}]}}}";
  ASSERT_FALSE(static_cast<bool>(deserializeJson(payload, raw))) << "fixture must parse";
  const std::string text = backup::buildEnvelope(payload.as<JsonVariantConst>(), "r102", "2026-11-09T20:00:00+0800");
  ASSERT_FALSE(text.empty());
  JsonDocument doc;
  ASSERT_FALSE(static_cast<bool>(deserializeJson(doc, text)));
  EXPECT_EQ(backup::parseEnvelope(doc), backup::Reject::None);
  EXPECT_TRUE(doc["payload"]["settings"]["dictionaryName"].is<const char*>());
}

TEST(BackupEnvelope, TamperedPayloadIsRejectedBySha) {
  JsonDocument doc = makeEnvelope(/*tamper=*/true);
  EXPECT_EQ(backup::parseEnvelope(doc), backup::Reject::BadSha);
}

TEST(BackupEnvelope, SchemaFromTheFutureIsRejected) {
  JsonDocument doc = makeEnvelope();
  doc["crosspoint_backup"]["schema"] = 2;
  EXPECT_EQ(backup::parseEnvelope(doc), backup::Reject::SchemaTooNew);
}

TEST(BackupEnvelope, WrongKindIsRejected) {
  JsonDocument doc = makeEnvelope();
  doc["crosspoint_backup"]["kind"] = "settings-only";
  EXPECT_EQ(backup::parseEnvelope(doc), backup::Reject::BadKind);
}

TEST(BackupEnvelope, TruncatedJsonIsRejected) {
  JsonDocument doc;
  const auto err = deserializeJson(doc, "{\"crosspoint_backup\":{\"schema\":1,\"kin");
  EXPECT_TRUE(static_cast<bool>(err));
  // Whatever partial tree the parser left behind, the envelope check must not
  // report a usable backup.
  EXPECT_NE(backup::parseEnvelope(doc), backup::Reject::None);
}

TEST(BackupEnvelope, MissingEnvelopeObjectIsRejected) {
  JsonDocument doc;
  deserializeJson(doc, "{\"payload\":{}}");
  EXPECT_EQ(backup::parseEnvelope(doc), backup::Reject::NotEnvelope);
}

TEST(BackupSanitize, OutOfRangeEnumIsClampedNotApplied) {
  JsonDocument in;
  deserializeJson(in, "{\"uiTheme\":99,\"sleepScreen\":-4,\"pageTurnGesture\":7}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {"Literata"}, out);
  EXPECT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(out["uiTheme"].as<int>(), 5);
  EXPECT_EQ(out["sleepScreen"].as<int>(), 0);
  EXPECT_EQ(out["pageTurnGesture"].as<int>(), 3);
  ASSERT_EQ(r.notes.size(), 3u);
}

TEST(BackupSanitize, MenuAntiAliasingSurvivesRoundTripAndIsClamped) {
  // r161: a new 0/1 field is silently dropped from every backup unless it has a
  // BackupProfile row — this is the regression test for that.
  JsonDocument in;
  deserializeJson(in, "{\"menuAntiAliasing\":0,\"textAntiAliasing\":1}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {"Literata"}, out);
  EXPECT_EQ(r.reason, backup::Reject::None);
  ASSERT_TRUE(out["menuAntiAliasing"].is<int>());
  EXPECT_EQ(out["menuAntiAliasing"].as<int>(), 0);
  EXPECT_EQ(out["textAntiAliasing"].as<int>(), 1);

  JsonDocument bad;
  deserializeJson(bad, "{\"menuAntiAliasing\":7}");
  JsonDocument clamped;
  EXPECT_EQ(backup::sanitizeSettings(bad.as<JsonObjectConst>(), {"Literata"}, clamped).reason,
            backup::Reject::None);
  EXPECT_EQ(clamped["menuAntiAliasing"].as<int>(), 1);
}

TEST(BackupSanitize, UnknownFontRejectsTheWholeFile) {
  JsonDocument in;
  deserializeJson(in, "{\"sdFontFamilyName\":\"Comic Sans\"}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {"Literata", "Atkinson"}, out);
  EXPECT_EQ(r.reason, backup::Reject::UnknownFont);
}

TEST(BackupSanitize, KnownFontAndBuiltinEmptyAreAccepted) {
  JsonDocument in;
  deserializeJson(in, "{\"sdFontFamilyName\":\"Literata\"}");
  JsonDocument out;
  EXPECT_EQ(backup::sanitizeSettings(in.as<JsonObjectConst>(), {"Literata"}, out).reason,
            backup::Reject::None);
  EXPECT_STREQ(out["sdFontFamilyName"].as<const char*>(), "Literata");

  JsonDocument in2;
  deserializeJson(in2, "{\"sdFontFamilyName\":\"\"}");
  JsonDocument out2;
  EXPECT_EQ(backup::sanitizeSettings(in2.as<JsonObjectConst>(), {}, out2).reason, backup::Reject::None);
}

TEST(BackupSanitize, WrongTypeIsRejected) {
  JsonDocument in;
  deserializeJson(in, "{\"uiTheme\":\"dark\"}");
  JsonDocument out;
  EXPECT_EQ(backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out).reason, backup::Reject::InvalidField);
}

TEST(BackupSanitize, MalformedLanguageIsRejected) {
  JsonDocument in;
  deserializeJson(in, "{\"language\":\"english\"}");
  JsonDocument out;
  EXPECT_EQ(backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out).reason, backup::Reject::InvalidField);
}

TEST(BackupSanitize, UnknownKeysAreDropped) {
  JsonDocument in;
  deserializeJson(in, "{\"uiTheme\":1,\"madeUpKey\":123,\"anotherFuture\":{\"x\":1}}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out);
  EXPECT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(r.droppedKeys, 2);
  EXPECT_FALSE(out["madeUpKey"].is<int>());
  EXPECT_EQ(out["uiTheme"].as<int>(), 1);
}

TEST(BackupSanitize, MissingSettingsSectionIsRejected) {
  JsonDocument out;
  EXPECT_EQ(backup::sanitizeSettings(JsonObjectConst(), {}, out).reason, backup::Reject::MissingSettings);
}

// ---------------------------------------------------------------------------
// r162 FIX5 — the restore clamp table.
//
// r161 clamped wordSpacing / screenMargin / characterSpacing to 0..5, so a
// restore of a perfectly ordinary profile wrote word spacing 5 % (default 100)
// and a 5 px margin (default 5..40 px), and dropped eight persisted keys that
// had no rule at all. These are the two regressions, one test each.
// ---------------------------------------------------------------------------
TEST(BackupSanitize, R162NonDefaultValuesSurviveRoundTripUnchanged) {
  JsonDocument in;
  deserializeJson(in,
                  "{\"wordSpacing\":175,\"characterSpacing\":0,\"screenMargin\":30,"
                  "\"powerOffScreen\":3,\"sideButtonLayout\":4,"
                  "\"homeButtonTapAction\":0,\"homeButtonDoubleTapAction\":4,\"homeButtonLongPressAction\":9,"
                  "\"bootKeyTapAction\":2,\"bootKeyLongPressAction\":0,"
                  "\"opdsDownloadFolder\":\"Books/Downloaded\"}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out);
  ASSERT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(r.notes.size(), 0u) << (r.notes.empty() ? "" : r.notes.front());
  EXPECT_EQ(out["wordSpacing"].as<int>(), 175);
  EXPECT_EQ(out["characterSpacing"].as<int>(), 0);
  EXPECT_EQ(out["screenMargin"].as<int>(), 30);
  EXPECT_EQ(out["powerOffScreen"].as<int>(), 3);
  EXPECT_EQ(out["sideButtonLayout"].as<int>(), 4);
  EXPECT_EQ(out["homeButtonTapAction"].as<int>(), 0);
  EXPECT_EQ(out["homeButtonDoubleTapAction"].as<int>(), 4);
  EXPECT_EQ(out["homeButtonLongPressAction"].as<int>(), 9);
  EXPECT_EQ(out["bootKeyTapAction"].as<int>(), 2);
  EXPECT_EQ(out["bootKeyLongPressAction"].as<int>(), 0);
  EXPECT_STREQ(out["opdsDownloadFolder"].as<const char*>(), "Books/Downloaded");
}

// r163: Browse Files' "show file extensions" preference is hand-persisted (it has
// no SettingsList row), which is exactly the shape r162's regressions came from:
// a new setting whose key has no rule is silently DROPPED on restore.
TEST(BackupSanitize, R163ShowFileExtensionsHasARule) {
  JsonDocument in;
  deserializeJson(in, "{\"showFileExtensions\":1}");
  JsonDocument out;
  auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out);
  ASSERT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(r.droppedKeys, 0) << "a new persisted key with no rule is dropped on restore";
  ASSERT_TRUE(out["showFileExtensions"].is<int>());
  EXPECT_EQ(out["showFileExtensions"].as<int>(), 1);

  JsonDocument bad;
  deserializeJson(bad, "{\"showFileExtensions\":7}");
  JsonDocument clamped;
  r = backup::sanitizeSettings(bad.as<JsonObjectConst>(), {}, clamped);
  ASSERT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(clamped["showFileExtensions"].as<int>(), 1);  // clamped to the toggle's 0..1
  EXPECT_EQ(r.notes.size(), 1u);

  JsonDocument off;
  deserializeJson(off, "{\"showFileExtensions\":0}");
  JsonDocument offOut;
  r = backup::sanitizeSettings(off.as<JsonObjectConst>(), {}, offOut);
  ASSERT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(offOut["showFileExtensions"].as<int>(), 0);
}

// r163: "back short -> file browser" is gone -- the row, the field and the rule. A backup written
// by an older build still carries the key, and restoring one must not fail: sanitizeSettings drops
// a key this build does not know, counts it, and reports Reject::None. That is the whole migration.
TEST(BackupSanitize, R163BackShortToFileBrowserIsDroppedNotRejected) {
  JsonDocument in;
  deserializeJson(in, "{\"backShortToFileBrowser\":1,\"screenMargin\":20}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out);
  EXPECT_EQ(r.reason, backup::Reject::None) << "an old backup must still restore";
  EXPECT_EQ(r.droppedKeys, 1) << "the retired key is dropped and counted, not applied";
  EXPECT_FALSE(out["backShortToFileBrowser"].is<int>()) << "the removed field must not be resurrected";
  ASSERT_TRUE(out["screenMargin"].is<int>()) << "the rest of the file still restores";
  EXPECT_EQ(out["screenMargin"].as<int>(), 20);
}

TEST(BackupSanitize, R162OutOfRangeClampsToTheFieldsRealBounds) {
  JsonDocument in;
  deserializeJson(in,
                  "{\"wordSpacing\":500,\"characterSpacing\":9,\"screenMargin\":99,"
                  "\"powerOffScreen\":7,\"sideButtonLayout\":9,\"homeButtonTapAction\":99,"
                  "\"bootKeyTapAction\":7}");
  JsonDocument out;
  const auto r = backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out);
  ASSERT_EQ(r.reason, backup::Reject::None);
  EXPECT_EQ(out["wordSpacing"].as<int>(), 200);      // WORD_SPACING_MAX, not 5
  EXPECT_EQ(out["characterSpacing"].as<int>(), 4);   // 5 enum slots
  EXPECT_EQ(out["screenMargin"].as<int>(), 40);      // SCREEN_MARGIN_MAX, not 5
  EXPECT_EQ(out["powerOffScreen"].as<int>(), 4);
  EXPECT_EQ(out["sideButtonLayout"].as<int>(), 4);
  EXPECT_EQ(out["homeButtonTapAction"].as<int>(), 11);  // HomeButtonAction::Count - 1
  EXPECT_EQ(out["bootKeyTapAction"].as<int>(), 2);      // BOOT_KEY_ACTION_COUNT - 1
  EXPECT_EQ(r.notes.size(), 7u);

  // The lower edge matters too: a 0 % word spacing / 0 px margin is not a legal
  // value for these VALUE-type fields, so it is raised, not carried through.
  JsonDocument low;
  deserializeJson(low, "{\"wordSpacing\":0,\"screenMargin\":0}");
  JsonDocument lowOut;
  ASSERT_EQ(backup::sanitizeSettings(low.as<JsonObjectConst>(), {}, lowOut).reason, backup::Reject::None);
  EXPECT_EQ(lowOut["wordSpacing"].as<int>(), 50);
  EXPECT_EQ(lowOut["screenMargin"].as<int>(), 5);
}

TEST(BackupSanitize, R162OpdsFolderAccepts63BytesAndRejects64) {
  const std::string ok(63, 'a');
  const std::string bad(64, 'b');
  JsonDocument in;
  in["opdsDownloadFolder"] = ok;
  JsonDocument out;
  ASSERT_EQ(backup::sanitizeSettings(in.as<JsonObjectConst>(), {}, out).reason, backup::Reject::None);
  EXPECT_EQ(std::string(out["opdsDownloadFolder"].as<const char*>()), ok);

  JsonDocument tooLong;
  tooLong["opdsDownloadFolder"] = bad;
  JsonDocument dropped;
  EXPECT_EQ(backup::sanitizeSettings(tooLong.as<JsonObjectConst>(), {}, dropped).reason,
            backup::Reject::InvalidField);
}

TEST(BackupRotation, KeepsNewestAndDeletesOldestFirst) {
  std::vector<std::string> names = {
      "backup-r102-20261101-0900.json", "backup-r102-20261109-2000.json",
      "backup-r102-20261108-1800.json", "backup-r102-20261105-1200.json",
      "backup-r102-20261107-1500.json", "backup-r102-20261102-1000.json",
      "backup-r102-20261103-1100.json"};
  const auto doomed = backup::rotationPlan(names, 5);
  ASSERT_EQ(doomed.size(), 2u);
  EXPECT_EQ(doomed[0], "backup-r102-20261101-0900.json");
  EXPECT_EQ(doomed[1], "backup-r102-20261102-1000.json");
}

TEST(BackupRotation, NeverDeletesAFileItDidNotWrite) {
  std::vector<std::string> names = {"settings.json", "notes.txt",
                                   "backup-r102-20261101-0900.json", "backup-r102-20261102-1000.json"};
  const auto doomed = backup::rotationPlan(names, 1);
  ASSERT_EQ(doomed.size(), 1u);
  EXPECT_EQ(doomed[0], "backup-r102-20261101-0900.json");
}

TEST(BackupNames, ImportOnlyAcceptsBackupNames) {
  EXPECT_TRUE(backup::isBackupFileName("backup-r102-20261109-2000.json"));
  EXPECT_TRUE(backup::isBackupFileName("backup-R102-20261109-2000.json"));
  EXPECT_FALSE(backup::isBackupFileName("../settings.json"));
  EXPECT_FALSE(backup::isBackupFileName("settings.json"));
  EXPECT_FALSE(backup::isBackupFileName("backup-.json"));
  EXPECT_EQ(backup::backupStampOf("backup-r102-20261109-2000.json"), "20261109-2000");
}

TEST(BackupNames, ProgressDirectoriesCannotEscapeCrosspoint) {
  EXPECT_TRUE(backup::isBookCacheDirName("epub_1234567"));
  EXPECT_TRUE(backup::isBookCacheDirName("xtc_99"));
  EXPECT_FALSE(backup::isBookCacheDirName("epub_../x"));
  EXPECT_FALSE(backup::isBookCacheDirName("epub_"));
  EXPECT_FALSE(backup::isBookCacheDirName("pdf_1"));
  EXPECT_FALSE(backup::isBookCacheDirName("../../etc"));
}

TEST(BackupHex, RoundTripAndRejectGarbage) {
  const uint8_t bytes[] = {0x00, 0x02, 0xff, 0x10, 0x40};
  std::string hex;
  ASSERT_TRUE(backup::hexEncodeBytes(bytes, sizeof(bytes), hex));
  EXPECT_EQ(hex, "0002ff1040");
  std::vector<uint8_t> back;
  ASSERT_TRUE(backup::hexDecodeBytes(hex, back));
  ASSERT_EQ(back.size(), 5u);
  EXPECT_EQ(back[2], 0xff);
  EXPECT_FALSE(backup::hexDecodeBytes("0002f", back));  // odd length
  EXPECT_FALSE(backup::hexDecodeBytes("zz", back));
}

// ---- r163 B6: the vocabulary history's restore bound ------------------------

TEST(BackupVocab, KeepsOrderAndBothFields) {
  JsonDocument in;
  deserializeJson(in, R"({"words":[{"w":"apple","b":"Book One"},{"w":"birch","b":""}]})");
  JsonDocument out;
  const auto r = backup::sanitizeVocabHistory(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 2u);
  EXPECT_EQ(r.dropped, 0u);
  ASSERT_EQ(out["words"].size(), 2u);
  EXPECT_STREQ(out["words"][0]["w"] | "", "apple");
  EXPECT_STREQ(out["words"][0]["b"] | "", "Book One");
  EXPECT_STREQ(out["words"][1]["w"] | "", "birch");
  EXPECT_STREQ(out["words"][1]["b"] | "", "");
}

TEST(BackupVocab, CapsAtTheStoreBound) {
  JsonDocument in;
  JsonArray arr = in["words"].to<JsonArray>();
  for (size_t i = 0; i < backup::kMaxVocabEntries + 5; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["w"] = "word";
    o["b"] = "book";
  }
  JsonDocument out;
  const auto r = backup::sanitizeVocabHistory(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, backup::kMaxVocabEntries);
  EXPECT_EQ(r.dropped, 5u);
  EXPECT_EQ(out["words"].size(), backup::kMaxVocabEntries);
}

TEST(BackupVocab, DropsAnEntryWithoutAWord) {
  JsonDocument in;
  deserializeJson(in, R"({"words":[{"w":"","b":"book"},{"b":"book"},{"w":"kept","b":"book"}]})");
  JsonDocument out;
  const auto r = backup::sanitizeVocabHistory(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 1u);
  EXPECT_EQ(r.dropped, 2u);
  EXPECT_EQ(out["words"].size(), 1u);
  EXPECT_STREQ(out["words"][0]["w"] | "", "kept");
}

TEST(BackupVocab, ClampsLongFieldsToTheStoreLimits) {
  JsonDocument in;
  JsonObject o = in["words"].add<JsonObject>();
  o["w"] = std::string(200, 'w').c_str();
  o["b"] = std::string(200, 'b').c_str();
  JsonDocument out;
  ASSERT_EQ(backup::sanitizeVocabHistory(in.as<JsonObjectConst>(), out).kept, 1u);
  const std::string word = out["words"][0]["w"] | "";
  const std::string book = out["words"][0]["b"] | "";
  EXPECT_EQ(word.size(), backup::kMaxVocabWordChars - 1);
  EXPECT_EQ(book.size(), backup::kMaxVocabBookChars - 1);
}

TEST(BackupVocab, ClampDoesNotSplitAUtf8Sequence) {
  // Three two-byte codepoints followed by more: cutting at an odd byte must back off
  // to the codepoint boundary rather than store half a character.
  JsonDocument in;
  JsonObject o = in["words"].add<JsonObject>();
  std::string word;
  for (int i = 0; i < 40; ++i) word += "\xc3\xa9";  // 'é'
  o["w"] = word.c_str();
  JsonDocument out;
  ASSERT_EQ(backup::sanitizeVocabHistory(in.as<JsonObjectConst>(), out).kept, 1u);
  const std::string stored = out["words"][0]["w"] | "";
  EXPECT_EQ(stored.size() % 2, 0u);  // an even byte count cannot end mid-sequence here
  EXPECT_LE(stored.size(), backup::kMaxVocabWordChars - 1);
}

TEST(BackupVocab, MissingSectionIsAnEmptyListNotAFailure) {
  JsonDocument in;
  JsonDocument out;
  const auto r = backup::sanitizeVocabHistory(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 0u);
  EXPECT_EQ(r.dropped, 0u);
  EXPECT_TRUE(out["words"].is<JsonArray>());
  EXPECT_EQ(out["words"].size(), 0u);
}

// ---- r163 3.3: the reading statistics' restore bound -----------------------

TEST(BackupReadingStats, KeepsBooksAndTotals) {
  JsonDocument in;
  deserializeJson(in, R"({"books":[{"path":"/books/a.epub","title":"A","pages":120,"seconds":3600},
                                   {"path":"/books/b.epub","title":"B","pages":7,"seconds":30}],
                          "pages":127,"seconds":3630})");
  JsonDocument out;
  const auto r = backup::sanitizeReadingStats(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 2u);
  EXPECT_EQ(r.dropped, 0u);
  EXPECT_EQ(r.clamped, 0u);
  ASSERT_EQ(out["books"].size(), 2u);
  EXPECT_STREQ(out["books"][0]["path"] | "", "/books/a.epub");
  EXPECT_STREQ(out["books"][1]["title"] | "", "B");
  EXPECT_EQ(out["books"][0]["pages"] | 0, 120);
  EXPECT_EQ(out["books"][1]["seconds"] | 0, 30);
  EXPECT_EQ(out["pages"] | 0, 127);
  EXPECT_EQ(out["seconds"] | 0, 3630);
}

TEST(BackupReadingStats, CapsAtTheStoreBound) {
  JsonDocument in;
  JsonArray arr = in["books"].to<JsonArray>();
  for (size_t i = 0; i < backup::kMaxReadingStatBooks + 4; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["path"] = "/books/x.epub";
    o["pages"] = 1;
    o["seconds"] = 60;
  }
  JsonDocument out;
  const auto r = backup::sanitizeReadingStats(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, backup::kMaxReadingStatBooks);
  EXPECT_EQ(r.dropped, 4u);
  EXPECT_EQ(out["books"].size(), backup::kMaxReadingStatBooks);
}

TEST(BackupReadingStats, DropsABookWithoutAPath) {
  JsonDocument in;
  deserializeJson(in, R"({"books":[{"path":"","title":"Nameless"},{"title":"Also Nameless"},
                                   {"path":"/books/kept.epub","title":"Kept"}]})");
  JsonDocument out;
  const auto r = backup::sanitizeReadingStats(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 1u);
  EXPECT_EQ(r.dropped, 2u);
  ASSERT_EQ(out["books"].size(), 1u);
  EXPECT_STREQ(out["books"][0]["path"] | "", "/books/kept.epub");
}

TEST(BackupReadingStats, ClampsWildCountersAndCountsThem) {
  JsonDocument in;
  JsonObject o = in["books"].add<JsonObject>();
  o["path"] = "/books/wild.epub";
  o["pages"] = 99000000;   // past kMaxReadingStatPages
  o["seconds"] = -5;       // negative
  in["pages"] = 99000000;
  in["seconds"] = 990000000;
  JsonDocument out;
  const auto r = backup::sanitizeReadingStats(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 1u);
  EXPECT_EQ(out["books"][0]["pages"] | 0, static_cast<int>(backup::kMaxReadingStatPages));
  EXPECT_EQ(out["books"][0]["seconds"] | 0, 0);
  EXPECT_EQ(out["pages"] | 0, static_cast<int>(backup::kMaxReadingStatPages));
  EXPECT_EQ(out["seconds"] | 0, static_cast<int>(backup::kMaxReadingStatSeconds));
  // One clamp for the book's counters, one for each clipped total.
  EXPECT_EQ(r.clamped, 3u);
}

TEST(BackupReadingStats, ClampsLongFieldsToTheStoreLimits) {
  JsonDocument in;
  JsonObject o = in["books"].add<JsonObject>();
  o["path"] = std::string(400, 'p').c_str();
  o["title"] = std::string(400, 't').c_str();
  JsonDocument out;
  ASSERT_EQ(backup::sanitizeReadingStats(in.as<JsonObjectConst>(), out).kept, 1u);
  const std::string path = out["books"][0]["path"] | "";
  const std::string title = out["books"][0]["title"] | "";
  EXPECT_EQ(path.size(), backup::kMaxReadingStatPathChars - 1);
  EXPECT_EQ(title.size(), backup::kMaxReadingStatTitleChars - 1);
}

TEST(BackupReadingStats, MissingSectionIsEmptyAndAbsentMeansUntouched) {
  JsonDocument in;
  JsonDocument out;
  const auto r = backup::sanitizeReadingStats(in.as<JsonObjectConst>(), out);
  EXPECT_EQ(r.kept, 0u);
  EXPECT_EQ(r.dropped, 0u);
  EXPECT_TRUE(out["books"].is<JsonArray>());
  EXPECT_EQ(out["books"].size(), 0u);
  EXPECT_EQ(out["pages"] | -1, 0);
  EXPECT_EQ(out["seconds"] | -1, 0);
}

// The two modules spell the bound out twice (the backup library must not include
// firmware headers), so pin them together here rather than trusting the comments.
TEST(BackupReadingStats, BoundMatchesTheStoreConstant) {
  EXPECT_EQ(backup::kMaxReadingStatBooks, static_cast<size_t>(readingstats::kMaxBooks));
  EXPECT_EQ(backup::kMaxReadingStatPages, readingstats::kMaxPagesPerBook);
  EXPECT_EQ(backup::kMaxReadingStatSeconds, readingstats::kMaxSecondsPerBook);
}
