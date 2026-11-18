// JPEG1 host-test table — see JpegHwSelectCases.h.

#include "JpegHwSelectCases.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "JpegHwPath.h"

namespace {

struct CompSpec {
  int h;
  int v;
};

// A structurally valid JPEG header: SOI, a JFIF APP0, a DQT, the SOF, an optional
// DRI, the SOS, a few entropy bytes, and (optionally) an EOI. The scanner only
// reads markers up to SOS; the EOI is what jpegStreamHasEoi looks for.
std::vector<uint8_t> makeJpeg(uint8_t sofMarker, int w, int h, const std::vector<CompSpec>& comps, bool withEoi,
                              bool withDri = false, int mcuStride = 0) {
  std::vector<uint8_t> d;
  const auto u8 = [&](uint8_t v) { d.push_back(v); };
  const auto u16 = [&](int v) {
    d.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    d.push_back(static_cast<uint8_t>(v & 0xFF));
  };

  u16(0xFFD8);  // SOI

  // APP0 / JFIF
  u16(0xFFE0);
  u16(16);
  const char jfif[14] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
  d.insert(d.end(), jfif, jfif + 14);

  // DQT (64 entries, 8-bit precision)
  u16(0xFFDB);
  u16(67);
  u8(0x00);
  for (int i = 0; i < 64; i++) u8(static_cast<uint8_t>(i + 1));

  // SOF
  u16(0xFF00 | sofMarker);
  const int nf = static_cast<int>(comps.size());
  u16(8 + 3 * nf);
  u8(8);  // precision
  u16(h);
  u16(w);
  u8(static_cast<uint8_t>(nf));
  for (int i = 0; i < nf; i++) {
    u8(static_cast<uint8_t>(i + 1));                                    // component id
    u8(static_cast<uint8_t>((comps[i].h << 4) | (comps[i].v & 0x0F)));  // sampling
    u8(0);                                                             // quant table
  }

  if (withDri) {
    u16(0xFFDD);
    u16(4);
    u16(0);
  }

  // SOS
  u16(0xFFDA);
  u16(6 + 2 * nf);
  u8(static_cast<uint8_t>(nf));
  for (int i = 0; i < nf; i++) {
    u8(static_cast<uint8_t>(i + 1));
    u8(0x00);  // DC/AC table 0
  }
  u8(0x00);
  u8(0x3F);
  u8(0x00);

  // entropy-coded data (content is irrelevant to the scan, but FF bytes must be
  // stuffed exactly as an encoder would)
  for (int i = 0; i < (mcuStride > 0 ? mcuStride : 64); i++) {
    if (i % 37 == 0) {
      u8(0xFF);
      u8(0x00);
    } else {
      u8(static_cast<uint8_t>(i * 31));
    }
  }

  if (withEoi) {
    u8(0xFF);
    u8(0xD9);
  }
  return d;
}

// Run the bytes through the scanner + policy and describe what happened.
struct Outcome {
  JpegProbe probe;
  JpegDecodePath path = JpegDecodePath::Software;
  JpegHwReason why = JpegHwReason::NotProbed;
  bool scanned = false;
};

Outcome run(const std::vector<uint8_t>& bytes, size_t freePsram = 6u * 1024 * 1024) {
  Outcome o;
  JpegMarkerScan scan;
  scan.feed(bytes.data(), bytes.size());
  o.scanned = scan.probe().headerOk;
  o.probe = scan.probe();
  o.probe.hasEoi = jpegStreamHasEoi(bytes.data(), bytes.size());
  JpegHwLimits limits;
  o.path = jpegSelectPath(o.probe, bytes.size(), freePsram, limits, &o.why);
  return o;
}

const char* pathName(JpegDecodePath p) { return p == JpegDecodePath::Hardware ? "HW" : "SW"; }

void check(std::vector<JpegHwCaseResult>* out, const char* name, const Outcome& o, JpegDecodePath wantPath,
           JpegHwReason wantWhy) {
  JpegHwCaseResult r;
  r.name = name;
  r.pass = (o.path == wantPath) && (o.why == wantWhy);
  char buf[256];
  snprintf(buf, sizeof buf, "%dx%d c=%d samp=%dx%d/%dx%d %s -> %s (%s), wanted %s (%s)", o.probe.width,
           o.probe.height, o.probe.components, o.probe.sampH0, o.probe.sampV0, o.probe.sampH1, o.probe.sampV1,
           jpegCodingName(o.probe.coding), pathName(o.path), jpegHwReasonName(o.why), pathName(wantPath),
           jpegHwReasonName(wantWhy));
  r.detail = buf;
  out->push_back(r);
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::vector<uint8_t> d;
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) return d;
  uint8_t tmp[8192];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) d.insert(d.end(), tmp, tmp + n);
  fclose(f);
  return d;
}

}  // namespace

std::vector<JpegHwCaseResult> runJpegHwSelectCases(const std::string& repoRoot, JpegHwCaseCounts* counts) {
  std::vector<JpegHwCaseResult> out;
  const std::vector<CompSpec> y420 = {{2, 2}, {1, 1}, {1, 1}};
  const std::vector<CompSpec> y422 = {{2, 1}, {1, 1}, {1, 1}};
  const std::vector<CompSpec> y444 = {{1, 1}, {1, 1}, {1, 1}};
  const std::vector<CompSpec> grey = {{1, 1}};

  // ---- the accept case, and the three sampling variants -------------------
  // 800x600 is 1.46 MB of raster; 1200x1800 is 6.5 MB and therefore refused for
  // memory alone (see the "over budget" case below) — that is the real shape of
  // this feature, not an accident of the table.
  check(&out, "baseline 4:2:0 800x600", run(makeJpeg(0xC0, 800, 600, y420, true)), JpegDecodePath::Hardware,
        JpegHwReason::Eligible);
  check(&out, "baseline 4:2:2 900x700", run(makeJpeg(0xC0, 900, 700, y422, true)), JpegDecodePath::Hardware,
        JpegHwReason::Eligible);
  check(&out, "grey baseline 600x800", run(makeJpeg(0xC0, 600, 800, grey, true)), JpegDecodePath::Hardware,
        JpegHwReason::Eligible);
  check(&out, "SOF1 sequential 4:2:0 800x1000 (DRI present)", run(makeJpeg(0xC1, 800, 1000, y420, true, true)),
        JpegDecodePath::Hardware, JpegHwReason::Eligible);

  // ---- refusals ------------------------------------------------------------
  check(&out, "progressive SOF2", run(makeJpeg(0xC2, 1200, 1800, y420, true)), JpegDecodePath::Software,
        JpegHwReason::Progressive);
  check(&out, "arithmetic sequential SOF9", run(makeJpeg(0xC9, 800, 600, y420, true)), JpegDecodePath::Software,
        JpegHwReason::ArithmeticCoding);
  check(&out, "arithmetic progressive SOF10", run(makeJpeg(0xCA, 800, 600, y420, true)), JpegDecodePath::Software,
        JpegHwReason::ArithmeticCoding);
  check(&out, "lossless SOF3", run(makeJpeg(0xC3, 800, 600, y420, true)), JpegDecodePath::Software,
        JpegHwReason::CodingNotBaseline);
  check(&out, "CMYK 4 components", run(makeJpeg(0xC0, 800, 600, {{1, 1}, {1, 1}, {1, 1}, {1, 1}}, true)),
        JpegDecodePath::Software, JpegHwReason::ComponentCount);
  check(&out, "2 components", run(makeJpeg(0xC0, 800, 600, {{1, 1}, {1, 1}}, true)), JpegDecodePath::Software,
        JpegHwReason::ComponentCount);
  check(&out, "4:4:4 (driver would emit raw MCU blocks)", run(makeJpeg(0xC0, 800, 600, y444, true)),
        JpegDecodePath::Software, JpegHwReason::Sampling444);
  check(&out, "eccentric sampling 1x2", run(makeJpeg(0xC0, 800, 600, {{1, 2}, {1, 1}, {1, 1}}, true)),
        JpegDecodePath::Software, JpegHwReason::SamplingUnsupported);
  check(&out, "eccentric sampling 2x4", run(makeJpeg(0xC0, 800, 600, {{2, 4}, {1, 1}, {1, 1}}, true)),
        JpegDecodePath::Software, JpegHwReason::SamplingUnsupported);
  check(&out, "height over the software cap", run(makeJpeg(0xC0, 2048, 3073, y420, true)), JpegDecodePath::Software,
        JpegHwReason::DimensionsOutOfRange);
  check(&out, "zero dimensions", run(makeJpeg(0xC0, 0, 0, y420, true)), JpegDecodePath::Software,
        JpegHwReason::DimensionsOutOfRange);
  check(&out, "truncated (no EOI)", run(makeJpeg(0xC0, 800, 600, y420, false)), JpegDecodePath::Software,
        JpegHwReason::NoEoi);
  check(&out, "1400x1900 = 8.0 MB raster, over budget", run(makeJpeg(0xC0, 1400, 1900, y420, true)),
        JpegDecodePath::Software, JpegHwReason::StreamTooLarge);
  check(&out, "eligible but only 2 MB of PSRAM free", run(makeJpeg(0xC0, 800, 600, y420, true), 2u * 1024 * 1024),
        JpegDecodePath::Software, JpegHwReason::NoPsram);

  // ---- the budget boundary, computed from the policy's own constants -------
  {
    const JpegHwLimits limits;
    JpegProbe p;
    p.headerOk = true;
    p.width = 1008;
    p.height = 1008;
    p.components = 3;
    p.sampH0 = 2;
    p.sampV0 = 2;
    p.coding = JpegCoding::Baseline;
    p.hasEoi = true;
    const uint64_t outBytes = jpegHwOutputBytes(p);
    const size_t atLimit = static_cast<size_t>(limits.maxHwBytes - outBytes);
    // stream + raster == maxHwBytes must still be accepted (the check is "over").
    auto b = makeJpeg(0xC0, 1008, 1008, y420, true);
    b.resize(atLimit, 0x11);  // pad so size() == atLimit exactly
    b[atLimit - 2] = 0xFF;    // keep a well-formed EOI at the end
    b[atLimit - 1] = 0xD9;
    check(&out, "stream+raster exactly at the budget", run(b, 8u * 1024 * 1024), JpegDecodePath::Hardware,
          JpegHwReason::Eligible);
    auto b2 = makeJpeg(0xC0, 1008, 1008, y420, true);
    b2.resize(atLimit + 1, 0x11);
    b2[atLimit - 1] = 0xFF;
    b2[atLimit] = 0xD9;
    check(&out, "one byte over the budget", run(b2, 8u * 1024 * 1024), JpegDecodePath::Software,
          JpegHwReason::StreamTooLarge);
  }

  // ---- chunked feeding must agree with one-shot feeding --------------------
  {
    const auto bytes = makeJpeg(0xC0, 1234, 1777, y420, true, true);
    JpegMarkerScan whole;
    whole.feed(bytes.data(), bytes.size());
    bool same = true;
    for (size_t chunk = 1; chunk <= 7; chunk++) {
      JpegMarkerScan scan;
      for (size_t i = 0; i < bytes.size() && !scan.done() && !scan.failed(); i += chunk) {
        scan.feed(bytes.data() + i, std::min(chunk, bytes.size() - i));
      }
      const JpegProbe& a = whole.probe();
      const JpegProbe& b = scan.probe();
      if (a.width != b.width || a.height != b.height || a.components != b.components || a.sampH0 != b.sampH0 ||
          a.sampV0 != b.sampV0 || a.coding != b.coding || a.headerOk != b.headerOk || scan.done() != whole.done()) {
        same = false;
      }
    }
    JpegHwCaseResult r;
    r.name = "scanner chunk size 1..7 B agrees with one-shot";
    r.pass = same && whole.probe().headerOk;
    r.detail = same ? "identical probe for every chunk size" : "probe differs by chunk size";
    out.push_back(r);
  }

  // ---- garbage / non-JPEG --------------------------------------------------
  {
    const std::vector<uint8_t> notJpeg = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
    Outcome o = run(notJpeg);
    JpegHwCaseResult r;
    r.name = "PNG bytes are not probed as JPEG";
    r.pass = !o.scanned && o.path == JpegDecodePath::Software && o.why == JpegHwReason::NotProbed;
    char buf[128];
    snprintf(buf, sizeof buf, "scanned=%d -> %s (%s)", (int)o.scanned, pathName(o.path), jpegHwReasonName(o.why));
    r.detail = buf;
    out.push_back(r);
  }

  // ---- luma-diff accumulator ----------------------------------------------
  {
    JpegLumaDiff d;
    const uint8_t a[8] = {0, 10, 20, 30, 200, 250, 128, 64};
    const uint8_t b[8] = {0, 10, 20, 31, 199, 252, 128, 40};  // 0,0,0,1,1,2,0,24
    d.addRow(a, b, 8);
    JpegHwCaseResult r;
    r.name = "luma diff stats (|d| = 0,0,0,1,1,2,0,24)";
    const bool ok = d.pixels == 8 && d.differing == 4 && d.over2 == 1 && d.over4 == 1 && d.maxAbs == 24 &&
                    d.sumAbs == 28 && !d.matches();
    r.pass = ok;
    char buf[160];
    snprintf(buf, sizeof buf, "pixels=%llu differing=%llu over2=%llu over4=%llu max=%u sum=%llu matches=%d",
             (unsigned long long)d.pixels, (unsigned long long)d.differing, (unsigned long long)d.over2,
             (unsigned long long)d.over4, d.maxAbs, (unsigned long long)d.sumAbs, (int)d.matches());
    r.detail = buf;
    out.push_back(r);
  }
  {
    // A one-level IDCT difference everywhere must still pass the gate; a one-level
    // shift that is systematic (the whole plane off by 3) must not.
    JpegLumaDiff idct;
    JpegLumaDiff shifted;
    uint8_t a[64];
    uint8_t b[64];
    uint8_t c[64];
    for (int i = 0; i < 64; i++) {
      a[i] = static_cast<uint8_t>(i * 3);
      b[i] = static_cast<uint8_t>(i * 3 + (i & 1));  // +1 on half the pixels
      c[i] = static_cast<uint8_t>(i * 3 + 3);
    }
    idct.addRow(a, b, 64);
    shifted.addRow(a, c, 64);
    JpegHwCaseResult r;
    r.name = "gate: +1 on half passes, +3 everywhere fails";
    r.pass = idct.matches() && !shifted.matches();
    char buf[160];
    snprintf(buf, sizeof buf, "idct(mean=%.2f max=%u matches=%d) shifted(mean=%.2f max=%u matches=%d)", idct.meanAbs(),
             idct.maxAbs, (int)idct.matches(), shifted.meanAbs(), shifted.maxAbs, (int)shifted.matches());
    r.detail = buf;
    out.push_back(r);
  }

  // ---- real files in the repo ---------------------------------------------
  // Header-only expectations: what the scanner says about them, and which path
  // those bytes select. These prove the scanner against files a real encoder wrote.
  struct RealCase {
    const char* fileRel;
    int w;
    int h;
    int comps;
    JpegCoding coding;
    JpegDecodePath path;
    JpegHwReason why;
  };
  const RealCase real[] = {
      {"/docs/images/cover.jpg", 2000, 2000, 3, JpegCoding::Baseline, JpegDecodePath::Software,
       JpegHwReason::Sampling444},
      {"/docs/images/comparison/menu.jpg", 3247, 2726, 3, JpegCoding::Baseline, JpegDecodePath::Software,
       JpegHwReason::DimensionsOutOfRange},
      {"/.pio/libdeps/metalio_eink4_plus/JPEGDEC/demo.jpg", 945, 707, 3, JpegCoding::Baseline,
       JpegDecodePath::Hardware, JpegHwReason::Eligible},
      {"/.pio/libdeps/metalio_eink4_plus/JPEGDEC/perf.jpg", 1272, 680, 3, JpegCoding::Baseline,
       JpegDecodePath::Hardware, JpegHwReason::Eligible},
  };
  for (const auto& rc : real) {
    const std::string full = repoRoot + rc.fileRel;
    const std::vector<uint8_t> bytes = readFile(full);
    JpegHwCaseResult r;
    r.name = std::string("real file ") + rc.fileRel;
    if (bytes.empty()) {
      r.skipped = true;
      r.detail = "not present in this checkout";
      out.push_back(r);
      continue;
    }
    Outcome o = run(bytes);
    const bool ok = o.probe.width == rc.w && o.probe.height == rc.h && o.probe.components == rc.comps &&
                    o.probe.coding == rc.coding && o.path == rc.path && o.why == rc.why;
    r.pass = ok;
    char buf[256];
    snprintf(buf, sizeof buf, "%dx%d c=%d %s %s (%s) bytes=%zu", o.probe.width, o.probe.height, o.probe.components,
             jpegCodingName(o.probe.coding), pathName(o.path), jpegHwReasonName(o.why), bytes.size());
    r.detail = buf;
    out.push_back(r);
  }

  if (counts) {
    counts->total = static_cast<int>(out.size());
    for (const auto& r : out) {
      if (r.skipped) {
        counts->skipped++;
      } else if (r.pass) {
        counts->passed++;
      } else {
        counts->failed++;
      }
    }
  }
  return out;
}
