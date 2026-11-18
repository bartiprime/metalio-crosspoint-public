// Host tests for htmlToPlainText(), used to render dictionary definitions that
// cannot be laid out as styled pages.

#include <gtest/gtest.h>

#include <string>

#include "HtmlToPlainText.h"

namespace {

TEST(HtmlToPlainText, StripsTagsAndKeepsText) {
  EXPECT_EQ(htmlToPlainText("<b>quixotic</b> <i>adj.</i>", '\x01'), "quixotic adj.");
  EXPECT_EQ(htmlToPlainText("<span class=\"x\">in span</span>", '\x01'), "in span");
  EXPECT_EQ(htmlToPlainText("plain", '\x01'), "plain");
  EXPECT_EQ(htmlToPlainText("", '\x01'), "");
}

TEST(HtmlToPlainText, BlockElementsBecomeBreaks) {
  EXPECT_EQ(htmlToPlainText("a<br>b", '\x01'), "a\nb");
  EXPECT_EQ(htmlToPlainText("<div>a</div><div>b</div>", '\x01'), "a\nb");
  EXPECT_EQ(htmlToPlainText("<p>a</p><p>b</p>", '\x01'), "a\n\nb");
  // Consecutive breaks do not stack up.
  EXPECT_EQ(htmlToPlainText("a<br><br><br>b", '\x01'), "a\nb");
}

TEST(HtmlToPlainText, HeadingsBecomeBoldLabelLines) {
  // tagBreak() lists h1-h6 among the heading names, so they must actually
  // reach that comparison -- a name scan that stops at the first digit
  // reads "h1" as "h" and the heading runs into the text after it.
  const char marker = '\x01';
  for (const char* tag : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
    const std::string html = std::string("<") + tag + ">title</" + tag + ">body";
    EXPECT_EQ(htmlToPlainText(html, marker), std::string(1, marker) + "title\nbody") << tag;
  }
  // The marker only means something at the start of a line, so a heading that
  // follows text still starts its own line.
  EXPECT_EQ(htmlToPlainText("lead<h2>title</h2>body", marker), "lead\n" + std::string(1, marker) + "title\nbody");
  EXPECT_EQ(htmlToPlainText("<hr>after", marker), "after");
}

TEST(HtmlToPlainText, DecodesEntities) {
  EXPECT_EQ(htmlToPlainText("Tom &amp; Jerry", '\x01'), "Tom & Jerry");
  EXPECT_EQ(htmlToPlainText("a&nbsp;b", '\x01'),
            "a\xC2\xA0"
            "b");
  EXPECT_EQ(htmlToPlainText("&#65;&#66;", '\x01'), "AB");
  EXPECT_EQ(htmlToPlainText("&#x2014;", '\x01'), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  EXPECT_EQ(htmlToPlainText("&notanentity;", '\x01'), "&notanentity;");
  EXPECT_EQ(htmlToPlainText("100% & up", '\x01'), "100% & up");
  EXPECT_EQ(htmlToPlainText("<p>Literal &lt;b&gt;word&lt;/b&gt; and &#60;tag&#62;</p>", '\x01'),
            "Literal <b>word</b> and <tag>");
}

TEST(HtmlToPlainText, TrimsSurroundingWhitespace) {
  EXPECT_EQ(htmlToPlainText("<p>only</p>", '\x01'), "only");
  EXPECT_EQ(htmlToPlainText("text   ", '\x01'), "text");
  EXPECT_EQ(htmlToPlainText("a\tb", '\x01'), "a b");
  EXPECT_EQ(htmlToPlainText("<br>a", '\x01'), "a");
}

TEST(HtmlToPlainText, SurvivesMalformedMarkup) {
  EXPECT_EQ(htmlToPlainText("a <b", '\x01'), "a <b");
  EXPECT_EQ(htmlToPlainText("x < y", '\x01'), "x < y");
  EXPECT_EQ(htmlToPlainText("<!-- comment -->kept", '\x01'), "kept");
}

// r163 B4: the conversion runs inside the caller's buffer. A dictionary
// definition is read into a std::string and moved in, so no second
// definition-sized allocation is made while converting it.
TEST(HtmlToPlainText, CompactsIntoTheCallersBuffer) {
  std::string html;
  html.reserve(8192);
  html = "<p>a definition with <b>markup</b> in it</p>";
  const char* const buffer = html.data();
  const size_t capacity = html.capacity();

  const std::string plain = htmlToPlainText(std::move(html), '\x01');
  EXPECT_EQ(plain, "a definition with markup in it");
  EXPECT_EQ(plain.data(), buffer) << "conversion allocated a second buffer";
  EXPECT_EQ(plain.capacity(), capacity);
}

// The failure mode of reusing a buffer: a shorter definition inheriting the
// tail of a longer one. The returned string is sized to the compacted length,
// and every byte the loop never wrote is beyond that length.
TEST(HtmlToPlainText, ReusedBufferLeavesNoStaleTail) {
  std::string longer = "<p>" + std::string(4000, 'x') + "</p>";
  std::string first = htmlToPlainText(std::move(longer), '\x01');
  EXPECT_EQ(first.size(), 4000u);
  ASSERT_GE(first.capacity(), 4000u);

  // Feed the same allocation a much shorter definition.
  first.assign("<p>tiny</p>");
  const std::string second = htmlToPlainText(std::move(first), '\x01');
  EXPECT_EQ(second, "tiny");
  EXPECT_EQ(second.size(), 4u);
  EXPECT_EQ(second.find('x'), std::string::npos) << "stale bytes survived the shorter conversion";

  // Same check with a marker heading, which writes a synthetic byte forward.
  std::string withHeading = "<h2>head</h2>body";
  const std::string converted = htmlToPlainText(std::move(withHeading), '\x01');
  EXPECT_EQ(converted.size(), 10u);
  EXPECT_EQ(converted, std::string("\x01") + "head\nbody");
}

}  // namespace
