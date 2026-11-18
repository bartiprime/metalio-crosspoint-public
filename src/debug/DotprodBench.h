#pragma once

// SIMD1 — the s8 dot-product benchmark (r101.21, corrected r103).
//
// Bench only: no product path calls this, and nothing in the image links it
// unless `exp simd` is typed. It measures esp-dsp's vectorised s8 kernel against
// its own ANSI twin over the same buffers, so the number is a ratio of two
// implementations of one function rather than a claim about a codebase.
//
// The question it answers is a PLANNING one: sanoTTS's RTF and TTS2's gate
// assume roughly the S3's ~4x. If this target reproduces it, that premise holds;
// if the twin is faster, the vector kernel is not selected for esp32s31 and the
// premise must be re-read (`dspi_dotprod_platform.h` gates on
// CONFIG_IDF_TARGET_ESP32S31 + CONFIG_DSP_OPTIMIZED).
//
// WHAT CHANGED IN r103 (SIMD1) AND WHY
//   The r101.21 build reported `ratio=33.84x` with `value kernel=-74 ansi=3`:
//   the two kernels disagreed, so the ratio was a quotient of two different
//   computations. A ratio is only meaningful when every timed shape returns the
//   SAME value from the vector kernel, its ANSI twin and an exact int32
//   reference. That is now the gate: the bench runs a table of shapes, compares
//   VALUES first, and prints the ratio only when all three agree on every shape.
//   Otherwise it prints VALUES DIFFER with both values and the shape, and
//   withholds the ratio -- the number a 34x claim was built on.
//
//   The descriptor is no longer written per-call: DotprodBenchMath.h derives it
//   from the vendored header's own semantics, and the host test
//   (test/dotprod_layout) checks that walk against the real ANSI kernel.
//
// It runs on the main task (the USB shell's `exp` kind) and takes no lock: the
// buffers are private to this translation unit and are not touched by anything
// else.

#include <cstddef>
#include <cstdint>

namespace dotprodb {

// One shape: count_x samples per row (a neural-inference reduction length K,
// kept a multiple of 16 so the arp4 kernel is eligible) over count_y rows.
struct ShapeResult {
  int countX = 0;
  int countY = 0;
  int refValue = 0;     // exact int32 dot, rounded and shifted like the library
  int kernelValue = 0;  // dspi_dotprod_s8 (arp4 on esp32s31)
  int ansiValue = 0;    // dspi_dotprod_s8_ansi
  uint32_t kernelUs = 0;
  uint32_t ansiUs = 0;
  bool fits = false;   // the exact result survives the int8 output at this shift
  bool agree = false;  // fits && kernel == ansi == ref
};

constexpr int kMaxShapes = 8;

struct Result {
  bool built = false;     // the esp-dsp header was on the include path
  bool fastPath = false;  // dspi_dotprod_s8 resolves to the arp4 kernel, not ansi
  bool ok = false;        // every shape returned ESP_OK and all values agree
  int iterations = 0;     // timed repetitions of each kernel, per shape
  int samples = 0;        // MACs of the largest timed shape
  int shapeCount = 0;
  int agreeCount = 0;
  uint32_t ansiUs = 0;  // summed over all timed shapes
  uint32_t optUs = 0;
  int ansiResult = 0;  // first shape, kept for the settings row's one-liner
  int optResult = 0;
  uint32_t ratioX100 = 0;  // ansiUs / optUs, x100; 0 unless ok AND fastPath
  ShapeResult shapes[kMaxShapes];
};

// `iterations` is clamped to 1..2000 and applies PER SHAPE.
Result run(int iterations);

// Writes the multi-line report (shape table + verdict) into `out`. Fixed buffer,
// no allocation: an exp bench must not fail on text formatting.
void formatReport(const Result& r, char* out, size_t cap);

}  // namespace dotprodb
