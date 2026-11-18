// SIMD1 — see src/debug/DotprodBench.h.
//
// WHY esp-dsp RATHER THAN A HAND-WRITTEN LOOP
//   The managed component is already in this build (idf/managed_components/
//   espressif__esp-dsp, linked as libespressif__esp-dsp.a), and on esp32s31 with
//   CONFIG_DSP_OPTIMIZED=y its dspi_dotprod_platform.h selects the arp4 kernel.
//   Comparing it against its own _ansi twin is the only comparison that isolates
//   the vector path: same signature, same inputs, same buffer layout, same
//   rounding.
//
// WHY THE VALUE GATE COMES FIRST
//   r101.21 printed `ratio=33.84x  value kernel=-74 ansi=3`: two different
//   computations, one quotient. A ratio is only worth printing when the vector
//   kernel, its ANSI twin and an exact int32 reference agree on EVERY timed
//   shape; otherwise the fast number is a property of the disagreement. The gate
//   below runs first and the timing numbers are printed either way, so the
//   refusal is diagnostic rather than silent.

#include "debug/DotprodBench.h"

#include <cstdarg>
#include <cstdio>

#include <esp_timer.h>

#include "dsp_err.h"
#include "dsp_types.h"
#include "dspi_dotprod.h"
#include "dspi_dotprod_platform.h"

#include "debug/DotprodBenchMath.h"

namespace dotprodb {
namespace {

// The header's own default for int8_t ("by default must be ... 7 for int8_t").
constexpr int kShift = 7;

// Largest shape in the table below (512 x 4 = 2048 MACs). Both buffers are
// static so a measurement never pays for an allocation; 2 KB each keeps the
// bench's .bss cost the same as r101.21's, and alignas(16) because the arp4
// kernel uses 128-bit loads.
constexpr int kMaxElements = 2048;
alignas(16) int8_t g_in[kMaxElements];
alignas(16) int8_t g_filter[kMaxElements];

struct ShapeSpec {
  int countX;
  int countY;
};

// Reduction length K x rows M. All K are multiples of 16 because that is the
// arp4 kernel's own eligibility test (dspi_dotprod_s8_arp4.S: `count_x & 15`
// must be zero and both step_x must be 1); a shape that fails it silently times
// the ANSI twin and would report "1x SIMD". K=64..512 with M=32..4 is the shape
// of a quantised GEMM's inner reduction, not a 3-element vector.
constexpr ShapeSpec kShapes[] = {
    {64, 32},   // 2048 MACs, the r101.21 shape
    {128, 16},  // 2048, K doubled / rows halved
    {256, 8},   // 2048
    {512, 4},   // 2048, the longest reduction
    {16, 64},   // 1024, the smallest K the arp4 kernel accepts
};
constexpr int kShapeCount = static_cast<int>(sizeof(kShapes) / sizeof(kShapes[0]));
static_assert(kShapeCount <= kMaxShapes, "kMaxShapes is too small for the shape table");

// True only when the macro in dspi_dotprod.h really resolves dspi_dotprod_s8 to
// the arp4 kernel. If it does not, both timed calls are the ANSI twin and any
// ratio is 1.0x by construction -- so it must not be printed as a SIMD result.
#if defined(dspi_dotprod_arp4_enabled) && (dspi_dotprod_arp4_enabled == 1)
constexpr bool kFastPathCompiled = true;
#else
constexpr bool kFastPathCompiled = false;
#endif

// image2d_t is the vendored struct; the field values come from the pure helper so
// the host test can check them against the real ANSI kernel.
void toImage(image2d_t* img, const ImageLayout& l, int8_t* data) {
  img->data = data;
  img->step_x = l.stepX;
  img->step_y = l.stepY;
  img->stride_x = l.strideX;
  img->stride_y = l.strideY;
  img->size_x = l.sizeX;
  img->size_y = l.sizeY;
}

// Deterministic, non-degenerate, and SMALL ON PURPOSE: even magnitudes in
// {-8,-6,-4,-2,2,4,6,8}, never zero. Two properties are wanted: (1) no zero, so a
// dropped MAC changes the result instead of hiding in a sum of zeros; (2) the
// accumulated sum stays inside what the int8 output can carry at shift 7
// (|sum| <= 16384), so an equality test compares the dot product and not the
// integer truncation of an out-of-range accumulator. A 2048-term sum of
// magnitudes <= 8 has sd ~ sqrt(2048)*20 ~ 900, i.e. the shifted value lands
// near +-7 -- large enough to differ between shapes, small enough to fit. The
// `fits` gate still excludes any shape whose exact sum does not fit, rather than
// comparing truncated bytes.
int8_t smallVal(uint32_t* s) {
  static const int8_t kSet[8] = {-8, -6, -4, -2, 2, 4, 6, 8};
  *s = *s * 1664525u + 1013904223u;
  return kSet[(*s >> 13) & 7u];
}

// Every shape gets its OWN stream: the LCG restarts here, so reusing one seed
// would make the 2048-MAC shapes sum over identical element pairs and return
// identical values -- four shapes "agreeing" with one another would then prove
// nothing about the descriptor.
void seed(int count, uint32_t stream) {
  uint32_t s = 0x12345678u ^ stream;
  for (int i = 0; i < count; ++i) {
    g_in[i] = smallVal(&s);
    g_filter[i] = smallVal(&s);
  }
}

}  // namespace

Result run(int iterations) {
  Result r;
  r.built = true;  // this file only compiles when the esp-dsp headers resolved
  r.fastPath = kFastPathCompiled;
  r.iterations = iterations < 1 ? 1 : (iterations > 2000 ? 2000 : iterations);
  r.shapeCount = kShapeCount;

  bool allAgree = true;
  for (int si = 0; si < kShapeCount; ++si) {
    const ShapeSpec& spec = kShapes[si];
    ShapeResult& sr = r.shapes[si];
    sr.countX = spec.countX;
    sr.countY = spec.countY;
    const int samples = spec.countX * spec.countY;
    if (samples > r.samples) r.samples = samples;
    if (samples > kMaxElements) {  // table mistake, never a silent overrun
      sr.fits = false;
      sr.agree = false;
      allAgree = false;
      continue;
    }

    seed(samples, 0x9E3779B9u * static_cast<uint32_t>(si + 1));

    // Both images are dense over one contiguous block: step 1 in X and Y,
    // stride_x = K (the row pitch, in elements), stride_y = M (rows).
    const ImageLayout layout = denseLayout(spec.countX, spec.countY);
    image2d_t inImg;
    image2d_t fImg;
    toImage(&inImg, layout, g_in);
    toImage(&fImg, layout, g_filter);

    if (!layoutInRange(layout, spec.countX, spec.countY) ||
        !arp4FastPath(layout.stepX, layout.stepX, spec.countX)) {
      // Not a shape this kernel will vectorise: never reported as a ratio.
      sr.fits = false;
      sr.agree = false;
      allAgree = false;
      continue;
    }

    const int32_t exact = dotRef(layout, g_in, layout, g_filter, spec.countX, spec.countY);
    sr.fits = int8ResultFits(exact, kShift);
    sr.refValue = static_cast<int>(shiftedValue(exact, kShift));

    int8_t ansiOut = 0;
    int8_t kernelOut = 0;
    const esp_err_t eAnsi =
        dspi_dotprod_s8_ansi(&inImg, &fImg, &ansiOut, spec.countX, spec.countY, kShift);
    const esp_err_t eKernel =
        dspi_dotprod_s8(&inImg, &fImg, &kernelOut, spec.countX, spec.countY, kShift);
    sr.ansiValue = static_cast<int>(ansiOut);
    sr.kernelValue = static_cast<int>(kernelOut);

    // The two calls above double as the warm-up: caches and the predictor are
    // primed for both implementations before either timer starts.
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < r.iterations; ++i) {
      dspi_dotprod_s8_ansi(&inImg, &fImg, &ansiOut, spec.countX, spec.countY, kShift);
    }
    const int64_t t1 = esp_timer_get_time();
    for (int i = 0; i < r.iterations; ++i) {
      dspi_dotprod_s8(&inImg, &fImg, &kernelOut, spec.countX, spec.countY, kShift);
    }
    const int64_t t2 = esp_timer_get_time();

    sr.ansiUs = static_cast<uint32_t>(t1 - t0);
    sr.kernelUs = static_cast<uint32_t>(t2 - t1);
    r.ansiUs += sr.ansiUs;
    r.optUs += sr.kernelUs;
    if (si == 0) {
      r.ansiResult = sr.ansiValue;
      r.optResult = sr.kernelValue;
    }

    sr.agree = sr.fits && eAnsi == ESP_OK && eKernel == ESP_OK &&
               sr.ansiValue == sr.refValue && sr.kernelValue == sr.refValue;
    if (!sr.agree) {
      allAgree = false;
    } else {
      ++r.agreeCount;
    }
  }

  // The gate. r.ok is what every caller keys off, and it is values-first: the
  // ratio exists only when the values were proven equal, so a caller that knows
  // only `ok` cannot accidentally publish a number the bench refused to trust.
  // `fastPath` is a separate, compile-time fact (arp4 really selected); with it
  // false the ratio stays 0 and formatReport says why, rather than pretending
  // ANSI-vs-ANSI is a 1.00x SIMD result.
  r.ok = allAgree;
  r.ratioX100 = (r.ok && r.fastPath && r.optUs > 0)
                    ? static_cast<uint32_t>((static_cast<uint64_t>(r.ansiUs) * 100ull) / r.optUs)
                    : 0u;
  return r;
}

void formatReport(const Result& r, char* out, size_t cap) {
  if (out == nullptr || cap == 0) {
    return;
  }
  out[0] = '\0';
  size_t used = 0;
  auto emit = [&](const char* fmt, ...) {
    if (used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);
    if (n > 0) {
      used += static_cast<size_t>(n);
      if (used >= cap) used = cap - 1;
    }
  };

  // The USB shell hands `exp` a 768-byte buffer (UsbShell.cpp: char out[768]), so
  // this report is deliberately compact: header + one short line per shape + a
  // verdict. The verdict is the part that matters, and it must never be the part
  // that gets truncated away.
  emit("exp simd: %d iters/shape, %d shapes (dense int8: step_x=1 stride_x=K step_y=1 "
       "stride_y=rows, shift=%d)\n",
       r.iterations, r.shapeCount, kShift);
  for (int i = 0; i < r.shapeCount && i < kMaxShapes; ++i) {
    const ShapeResult& s = r.shapes[i];
    emit("  %4dx%-4d ref=%4d kernel=%4d ansi=%4d  k=%5u us a=%7u us  %s\n", s.countX, s.countY,
         s.refValue, s.kernelValue, s.ansiValue, static_cast<unsigned>(s.kernelUs),
         static_cast<unsigned>(s.ansiUs),
         !s.fits ? "EXCLUDED (not eligible / int8 overflow)"
                 : (s.agree ? "agree" : "VALUES DIFFER"));
  }

  if (!r.fastPath) {
    emit("exp simd: dspi_dotprod_s8 IS the ANSI twin here (dspi_dotprod_arp4_enabled=0): "
         "no ratio, nothing was vectorised\n");
    return;
  }
  if (r.ok) {
    emit("ratio=%.2fx over %d/%d shapes; kernel, ANSI twin and int32 reference agreed on "
         "every value (a_us / k_us)\n",
         static_cast<double>(r.ratioX100) / 100.0, r.agreeCount, r.shapeCount);
    return;
  }

  // Refusal. Name the first shape that disagrees, with both values, so the next
  // reader does not have to re-run the bench to find out what mismatched.
  for (int i = 0; i < r.shapeCount && i < kMaxShapes; ++i) {
    const ShapeResult& s = r.shapes[i];
    if (s.agree) continue;
    emit("VALUES DIFFER: shape %dx%d ref=%d kernel=%d ansi=%d -- NO RATIO PRINTED (a quotient "
         "of two different computations is not a speedup)\n",
         s.countX, s.countY, s.refValue, s.kernelValue, s.ansiValue);
    return;
  }
  emit("VALUES DIFFER: no shape produced a comparable value triple -- NO RATIO PRINTED\n");
}

}  // namespace dotprodb
