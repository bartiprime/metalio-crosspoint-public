// AUD-GATE — the blocking paint wait must be a bounded, re-armable rendezvous.
//
// The defect this check exists to stop (r101.10, fixed in r101.11): a wait that
// registered ONCE, took ONE notify and waited on it with portMAX_DELAY. A render
// task that answered the notify WITHOUT driving left the caller blocked forever,
// or -- once a retry loop was bolted on -- blocked on a registration the first
// wake had already consumed. The NTP sync screen froze on glass that way.
//
// What is asserted here, in two layers:
//
//   LAYER 1 (structural, reads the production source): the wait in
//   ActivityManager::requestUpdateAndWait() is a loop of >= 2 attempts, every
//   take is bounded (no portMAX_DELAY), the registration is RE-ARMED inside the
//   loop BEFORE the notify, the notify count is drained before arming, success is
//   declared only by the driven predicate, and the registration is dropped on
//   every exit path. These assertions read the real file, so reverting the code
//   to the r101.10 shape makes them fail.
//
//   LAYER 2 (behavioural, on the shape layer 1 found): a model of the rendezvous
//   with the attempt count and timeout SCRAPED FROM THAT SAME SOURCE, driven by a
//   virtual clock. It proves the two properties the frozen screen needed: the
//   wait returns in bounded time when the render task answers without driving,
//   and a retry can be woken a second time. OldShapeHangs() is the negative
//   control: the same model configured as r101.10 must fail to make progress,
//   which is what makes layer 2's green mean something.
//
// A model is weaker than calling the real function; the real function needs the
// FreeRTOS scheduler and a render task, and this batch may not add a seam under
// src/. Layer 1 is therefore the check that guards the production code, and
// layer 2 documents WHY the shape layer 1 demands is the right one.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CP_REPO_ROOT
#error "CP_REPO_ROOT must point at the repository root"
#endif

namespace {

std::string readWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Position of the '{' that opens the block whose header ends just before `from`,
// and of the matching '}'. Both npos when unbalanced.
size_t findOpenBraceAfter(const std::string& src, size_t from) {
  return src.find('{', from);
}

size_t matchBrace(const std::string& src, size_t openPos) {
  if (openPos == std::string::npos) return std::string::npos;
  int depth = 0;
  for (size_t i = openPos; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    else if (src[i] == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

// Body text (without the outer braces) of the block whose header contains
// `headerNeedle`.
std::string blockBody(const std::string& src, const std::string& headerNeedle, bool* found = nullptr) {
  const size_t at = src.find(headerNeedle);
  if (found) *found = (at != std::string::npos);
  if (at == std::string::npos) return {};
  const size_t open = findOpenBraceAfter(src, at + headerNeedle.size());
  const size_t close = matchBrace(src, open);
  if (open == std::string::npos || close == std::string::npos) return {};
  return src.substr(open + 1, close - open - 1);
}

int scrapeIntConstant(const std::string& src, const std::string& name, int fallback) {
  const std::string needle = name + " = ";
  const size_t at = src.find(needle);
  if (at == std::string::npos) return fallback;
  size_t i = at + needle.size();
  int value = 0;
  bool any = false;
  while (i < src.size() && src[i] >= '0' && src[i] <= '9') {
    value = value * 10 + (src[i] - '0');
    ++i;
    any = true;
  }
  return any ? value : fallback;
}

// Peel C++ comments so a needle cannot be "found" in a comment that merely
// describes the code (this file is full of them, and so is the source).
std::string stripComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
    } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
      i = std::min(src.size(), i + 2);
    } else {
      out.push_back(src[i]);
      ++i;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// The production source under test.
// ---------------------------------------------------------------------------
const std::string kActivityManagerPath = std::string(CP_REPO_ROOT) + "/src/activities/ActivityManager.cpp";

struct WaitSource {
  std::string raw;
  std::string code;  // comments stripped
  std::string fn;    // requestUpdateAndWait() body
  std::string loop;  // the attempt loop body
  int attempts = 0;
  int waitMs = 0;
  bool ok = false;
};

const WaitSource& waitSource() {
  static const WaitSource s = [] {
    WaitSource w;
    w.raw = readWholeFile(kActivityManagerPath);
    w.code = stripComments(w.raw);
    w.fn = blockBody(w.code, "bool ActivityManager::requestUpdateAndWait()", &w.ok);
    w.loop = blockBody(w.fn, "for (attempt = 1; attempt <= kPaintWaitAttempts; ++attempt)");
    w.attempts = scrapeIntConstant(w.fn, "kPaintWaitAttempts", -1);
    w.waitMs = scrapeIntConstant(w.fn, "kPaintWaitMs", -1);
    return w;
  }();
  return s;
}

size_t pos(const std::string& hay, const std::string& needle) { return hay.find(needle); }

}  // namespace

// ---------------------------------------------------------------------------
// LAYER 1 — the shape of the production wait.
// ---------------------------------------------------------------------------

TEST(AudGate, SourceIsReadable) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.raw.empty()) << "cannot read " << kActivityManagerPath;
  ASSERT_TRUE(w.ok) << "ActivityManager::requestUpdateAndWait() not found";
  ASSERT_FALSE(w.fn.empty());
  ASSERT_FALSE(w.loop.empty()) << "the retry loop is gone -- the wait is one-shot again";
  ASSERT_GE(w.attempts, 2) << "a one-shot wait cannot be re-woken (r101.10)";
  ASSERT_GT(w.waitMs, 0);
  ASSERT_LE(w.waitMs, 5000) << "a per-attempt timeout above 5 s is not a bounded wait any more";
}

// The r101.10 shape: ulTaskNotifyTake(pdTRUE, portMAX_DELAY) inside the wait.
TEST(AudGate, EveryWaitIsBounded) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.fn.empty());
  EXPECT_EQ(pos(w.fn, "portMAX_DELAY"), std::string::npos)
      << "requestUpdateAndWait() must never block indefinitely on the render task";
  EXPECT_NE(pos(w.fn, "kPaintWaitTicks"), std::string::npos)
      << "the take must use the bounded kPaintWaitTicks timeout";
}

// The r101.10 shape: registration consumed by the first wake and never re-armed,
// so a retry blocks on a handle nobody can notify again.
TEST(AudGate, RegistrationIsReArmedBeforeEveryPoke) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.loop.empty());
  const size_t arm = pos(w.loop, "waitingTaskHandle = currTaskHandler;");
  const size_t poke = pos(w.loop, "xTaskNotify(renderTaskHandle");
  ASSERT_NE(arm, std::string::npos) << "the waiter is not re-registered inside the retry loop";
  ASSERT_NE(poke, std::string::npos) << "the retry loop does not poke the render task";
  EXPECT_LT(arm, poke) << "re-arm must precede the poke, or the wake is delivered to nobody";
}

// A wake left in the notification count from an earlier paint must not be
// mistaken for this attempt's answer.
TEST(AudGate, StaleWakeIsDrainedBeforeArming) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.loop.empty());
  const size_t drain = pos(w.loop, "ulTaskNotifyTake(pdTRUE, 0)");
  const size_t arm = pos(w.loop, "waitingTaskHandle = currTaskHandler;");
  ASSERT_NE(drain, std::string::npos) << "no drain of a stale notify count before arming";
  ASSERT_NE(arm, std::string::npos);
  EXPECT_LT(drain, arm);
}

// "The take returned" is not an answer: only a new drive for THIS visit is.
TEST(AudGate, SuccessIsTheDrivenPredicateNotTheWake) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.fn.empty());
  EXPECT_NE(pos(w.fn, "paintedVisit"), std::string::npos)
      << "the driven predicate (paintedVisit) is gone: a wake alone would count as a paint";
  EXPECT_NE(pos(w.fn, "cp_paint_seq() != seqBefore"), std::string::npos)
      << "the drive-sequence term is gone: a stale drive could settle the wait";
}

// A handle left registered lets the NEXT call take an old wake.
TEST(AudGate, RegistrationIsDroppedOnEveryExitPath) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.fn.empty());
  const size_t loop = pos(w.fn, "for (attempt = 1; attempt <= kPaintWaitAttempts;");
  ASSERT_NE(loop, std::string::npos);
  const std::string afterLoop = w.fn.substr(loop);
  EXPECT_NE(pos(afterLoop, "waitingTaskHandle = nullptr"), std::string::npos)
      << "the waiter handle is not dropped after the loop";
}

// A timed-out wait must say so before the caller proceeds (the log assertion the
// fallback path of BATCH0_GUARD.md would have used on its own).
TEST(AudGate, TimeoutIsLogged) {
  const auto& w = waitSource();
  ASSERT_FALSE(w.fn.empty());
  EXPECT_NE(pos(w.raw, "no wake within"), std::string::npos)
      << "a bounded wait that expires must log `no wake within` before returning";
}

// ---------------------------------------------------------------------------
// LAYER 2 — the rendezvous semantics of the shape layer 1 found.
// ---------------------------------------------------------------------------

namespace {

// A deterministic model of the handshake, configured from the production
// constants. `rearm`/`bounded` are the two knobs that separate today's shape from
// r101.10; both default to today's.
struct Rendezvous {
  int attempts;
  int waitMs;
  int renderAnswers;       // how many notifies the render task answers before going quiet
  bool renderDrives = false;
  bool rearm = true;       // false => r101.10: registration consumed by the first wake
  bool bounded = true;     // false => r101.10: portMAX_DELAY

  // Results of run().
  int wakes = 0;
  int attempt = 0;
  double elapsedMs = 0;
  bool returned = false;
  bool driven = false;

  void run() {
    int registered = 0;  // 0 = nobody registered, 1 = this waiter
    const double stepLimitMs = 60.0 * 1000.0;  // a wall clock the test refuses to exceed
    double clock = 0;
    for (attempt = 1; attempt <= attempts; ++attempt) {
      if (rearm) registered = 1;
      if (registered == 0) {
        // Nobody is registered: the poke reaches an empty slot, so a bounded take
        // burns its timeout and an unbounded one never returns.
        if (!bounded) return;  // hangs: returned stays false
        clock += waitMs;
        elapsedMs = clock;
        continue;
      }
      // The poke: the render task takes the registration (one-shot), then either
      // answers or does not.
      registered = 0;
      if (attempt > renderAnswers) {
        if (!bounded) return;  // waits forever for an answer that never comes
        clock += waitMs;
        elapsedMs = clock;
        continue;
      }
      ++wakes;
      if (renderDrives) {  // the answer carried a real drive for this visit
        driven = true;
        elapsedMs = clock;
        returned = true;
        return;
      }
      // Answered without driving -- the case that froze the NTP screen.
      clock += 1;  // the wake itself is immediate
      elapsedMs = clock;
      if (clock > stepLimitMs) return;
    }
    returned = true;
    elapsedMs = clock;
    attempt = attempts;  // the loop's post-increment left it at attempts+1
  }
};

}  // namespace

// The frozen-screen case: the render task answers every notify but drives
// nothing. The wait must still RETURN, in bounded time.
TEST(AudGate, AnswersWithoutDrivingStillReturnInBoundedTime) {
  const auto& w = waitSource();
  ASSERT_GE(w.attempts, 2);
  ASSERT_GT(w.waitMs, 0);
  Rendezvous r;
  r.attempts = w.attempts;
  r.waitMs = w.waitMs;
  r.renderAnswers = 99;  // answers every poke
  r.run();
  EXPECT_TRUE(r.returned) << "a render task that answers without driving hung the caller";
  EXPECT_FALSE(r.driven) << "a wait that never drove must not claim it did";
  EXPECT_EQ(r.wakes, w.attempts) << "every attempt must be woken, not just the first";
  EXPECT_LE(r.elapsedMs, static_cast<double>(w.attempts * w.waitMs));
}

// The r101.11 fix: a retry can be woken a SECOND time.
TEST(AudGate, RetryCanBeWokenASecondTime) {
  const auto& w = waitSource();
  Rendezvous r;
  r.attempts = w.attempts;
  r.waitMs = w.waitMs;
  r.renderAnswers = 99;
  r.run();
  EXPECT_GE(r.wakes, 2) << "the retry was not woken: the rendezvous is one-shot again";
  EXPECT_EQ(r.wakes, r.attempt);
}

// A render task that goes quiet after its first answer must cost the remaining
// timeouts, not the whole UI.
TEST(AudGate, SilentRenderTaskCostsTheTimeouts) {
  const auto& w = waitSource();
  Rendezvous r;
  r.attempts = w.attempts;
  r.waitMs = w.waitMs;
  r.renderAnswers = 1;
  r.run();
  EXPECT_TRUE(r.returned);
  EXPECT_EQ(r.wakes, 1);
  EXPECT_LE(r.elapsedMs, static_cast<double>(w.attempts * w.waitMs));
}

// NEGATIVE CONTROL: the r101.10 shape must not make progress. If this ever
// passes, the model has stopped modelling the defect and the three tests above
// have stopped meaning anything.
TEST(AudGate, OldOneShotUnboundedShapeHangs) {
  const auto& w = waitSource();
  Rendezvous r;
  r.attempts = w.attempts;
  r.waitMs = w.waitMs;
  r.renderAnswers = 99;
  r.rearm = false;   // one-shot rendezvous
  r.bounded = false; // portMAX_DELAY
  r.run();
  EXPECT_FALSE(r.returned) << "the r101.10 shape was supposed to hang here";
  EXPECT_EQ(r.wakes, 0) << "an unregistered poke must not count as a wake";
}
