/**
 * @brief MY_WAVEFORM：均衡完整波形与快速裁剪波形
 */

#include "my_waveform.h"

#include "my_waveform_du.h"
#include "my_waveform_gc.h"
#include "my_waveform_gl.h"
#include "my_waveform_trim.h"
#include "my_gray8_gc16.h"
#include "my_gray8_gl16.h"

// CP_VENDOR_LUT_ONLY (fast-paint LUT source). The app owns that switch; this file only
// honours it. See docs/engineering/REFRESH_POLICY_RICKYOS.md, "Fast-paint LUT source".
#include "../../../../../src/display/RefreshPolicy.h"

#include <assert.h>
#include <string.h>

#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "my_wf"

#define MY_FULL_DU_FRAMES 20
#define MY_FULL_GC16_FRAMES 48
#define MY_FULL_GL16_FRAMES 48
#define MY_TRIM_ERASE_MAX 11
#define MY_TRIM_SAT_CUT 5
#define MY_TRIM_WHITE_SAT_CUT 0
#define MY_TRIM_HOLD 3
#define MY_GC16_FRAMES MY_FULL_GC16_FRAMES
#define MY_GL16_FRAMES MY_FULL_GL16_FRAMES

static const EpdWaveformTempInterval my_waveform_intervals[] = {
    { .min = 0, .max = 50 },
};

static inline void lut_or(uint8_t (*data)[16][4], int f, int to, int from, int action) {
    data[f][to][from / 4] |= (uint8_t)(action << (6 - 2 * (from % 4)));
}

static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

// 源 DU 只认目标 0/15；中间灰 50/50，暗→黑、亮→白
static uint8_t s_du_data[MY_FULL_DU_FRAMES][16][4];
// Non-const so the FAST path can point its LUT at the vendor table itself
// (CP_VENDOR_LUT_ONLY) instead of the boot-time binary projection below.
static EpdWaveformPhases s_du_phases = {
    .phases = MY_FULL_DU_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_du_data[0],
};
static const EpdWaveformPhases* s_du_ranges[] = { &s_du_phases };
static const EpdWaveformMode s_du_mode = {
    .type = 1,
    .temp_ranges = 1,
    .range_data = &s_du_ranges[0],
};

static void complete_du_build(void) {
    memset(s_du_data, 0, sizeof(s_du_data));
    for (int to = 0; to < 16; to++) {
        const int to_bin = to < 8 ? 0 : 15;
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < MY_FULL_DU_FRAMES; f++) {
                const int action = lut_get(my_du_25_0_data, f, to_bin, from);
                if (action != 0) {
                    lut_or(s_du_data, f, to, from, action);
                }
            }
        }
    }
}

// 白底 15→15 源表全保持：挂在已有往白推的一相上再推 1 帧
static void gl16_white_tick(uint8_t (*data)[16][4], int frames) {
    int tick = -1;
    for (int f = frames - 1; f >= 0; f--) {
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) == 2) {
                tick = f;
                break;
            }
        }
        if (tick >= 0) {
            break;
        }
    }
    if (tick < 0) {
        tick = frames > 2 ? frames - 3 : 0;
    }
    lut_or(data, tick, 15, 15, 2);
}

static uint8_t s_gc16_data[MY_FULL_GC16_FRAMES][16][4];
static uint8_t s_gl16_data[MY_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases s_gc16_phases = {
    .phases = MY_GC16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gc16_data[0],
};
static const EpdWaveformPhases s_gl16_phases = {
    .phases = MY_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gl16_data[0],
};
static const EpdWaveformPhases* s_gc16_ranges[] = { &s_gc16_phases };
static const EpdWaveformPhases* s_gl16_ranges[] = { &s_gl16_phases };
static const EpdWaveformMode s_gc16_mode = {
    .type = 2,
    .temp_ranges = 1,
    .range_data = &s_gc16_ranges[0],
};
static const EpdWaveformMode s_gl16_mode = {
    .type = 5,
    .temp_ranges = 1,
    .range_data = &s_gl16_ranges[0],
};

static const EpdWaveformMode* s_modes[] = {
    &s_du_mode,
    &s_gc16_mode,
    &s_gl16_mode,
};

const EpdWaveform MY_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = s_modes,
    .temp_intervals = my_waveform_intervals,
};

static EpdWaveformPhases s_fast_gc_phases = { .phases = 36 };
static EpdWaveformPhases s_fast_gl_phases = { .phases = 37 };
static const EpdWaveformPhases* s_fast_gc_ranges[] = { &s_fast_gc_phases };
static const EpdWaveformPhases* s_fast_gl_ranges[] = { &s_fast_gl_phases };
static const EpdWaveformMode s_fast_gc_mode = {
    .type = 2, .temp_ranges = 1, .range_data = s_fast_gc_ranges,
};
static const EpdWaveformMode s_fast_gl_mode = {
    .type = 5, .temp_ranges = 1, .range_data = s_fast_gl_ranges,
};
static const EpdWaveformMode* s_fast_modes[] = {
    &s_du_mode, &s_fast_gc_mode, &s_fast_gl_mode,
};
static const EpdWaveform s_fast_waveform = {
    .num_modes = 3, .num_temp_ranges = 1,
    .mode_data = s_fast_modes, .temp_intervals = my_waveform_intervals,
};

const EpdWaveform* my_waveform_fast(void) {
    return s_fast_gc_phases.luts ? &s_fast_waveform : NULL;
}

static void short_du_build(void);
static void wipe_build(void);

void my_waveform_init(void) {
    static bool initialized;
    if (initialized) return;
    initialized = true;
    complete_du_build();
    short_du_build();
    wipe_build();
    memcpy(s_gc16_data, my_gc_25_0_data, sizeof(s_gc16_data));
    memcpy(s_gl16_data, my_gl_25_0_data, sizeof(s_gl16_data));
    gl16_white_tick(s_gl16_data, MY_GL16_FRAMES);

    // Build once in PSRAM. Switching profiles never modifies an active LUT.
#if CP_VENDOR_LUT_ONLY
    // Fast-paint LUT source (CP_VENDOR_LUT_ONLY = 1): the FAST path reads the vendor's
    // own DU table instead of complete_du_build()'s binary projection, and the fast
    // GC16/GL16 profile keeps the untrimmed 48/48 tables. Nothing is trimmed, so the
    // PSRAM copy the trim needed is never allocated. my_waveform_trim() itself, its
    // constants and the growable seams all stay in place for CP_VENDOR_LUT_ONLY=0.
    s_du_phases.luts = (const uint8_t*)&my_du_25_0_data[0];
    s_fast_gc_phases.phases = MY_FULL_GC16_FRAMES;
    s_fast_gc_phases.luts = (const uint8_t*)&s_gc16_data[0];
    s_fast_gl_phases.phases = MY_FULL_GL16_FRAMES;
    s_fast_gl_phases.luts = (const uint8_t*)&s_gl16_data[0];
    ESP_LOGI(TAG, "fast LUT = vendor (DU 20 phases, GC16/GL16 48 untrimmed)");
#else
    uint8_t (*data)[16][4] = heap_caps_malloc(
        sizeof(s_gc16_data) + sizeof(s_gl16_data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        ESP_LOGW(TAG, "fast waveform allocation failed; balanced retained");
        return;
    }
    const my_waveform_trim_t trim = {
        .erase_max = MY_TRIM_ERASE_MAX, .sat_cut = MY_TRIM_SAT_CUT,
        .white_sat_cut = MY_TRIM_WHITE_SAT_CUT, .hold = MY_TRIM_HOLD,
    };
    const int gc = my_waveform_trim(&my_gc_25_0, &trim, data);
    const int gl = my_waveform_trim(&my_gl_25_0, &trim, data + MY_FULL_GC16_FRAMES);
    assert(gc == s_fast_gc_phases.phases);
    assert(gl == s_fast_gl_phases.phases);
    gl16_white_tick(data + MY_FULL_GC16_FRAMES, gl);
    s_fast_gc_phases.luts = (const uint8_t*)data;
    s_fast_gl_phases.luts = (const uint8_t*)(data + MY_FULL_GC16_FRAMES);
#endif
}

// Extreme GL uses eight optical grays; GC always uses the 16-gray profiles.
static const EpdWaveformPhases s_gray8_gl_phases = {
    .phases = 30, .phase_times = NULL, .luts = (const uint8_t*)e0470_gray8_gl16_data,
};
static const EpdWaveformPhases* s_gray8_gl_ranges[] = { &s_gray8_gl_phases };
static const EpdWaveformMode s_gray8_gl_mode = { .type=5, .temp_ranges=1, .range_data=s_gray8_gl_ranges };
static const EpdWaveformMode* s_gray8_modes[] = { &s_gray8_gl_mode };
static const EpdWaveform s_gray8_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_gray8_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_gray8(void) { return &s_gray8_waveform; }

// Binary endpoints only: seven black pushes, eight white pushes. Equal gray
// pairs stay held; the worker uses this only for changed binary UI pixels.
static uint8_t s_short_du_data[8][16][4];
static const EpdWaveformPhases s_short_du_phases = {
    .phases=8, .phase_times=NULL, .luts=(const uint8_t*)s_short_du_data,
};
static const EpdWaveformPhases* s_short_du_ranges[] = { &s_short_du_phases };
static const EpdWaveformMode s_short_du_mode = { .type=1, .temp_ranges=1, .range_data=s_short_du_ranges };
static const EpdWaveformMode* s_short_du_modes[] = { &s_short_du_mode };
static const EpdWaveform s_short_du_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_short_du_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_short_du(void) { return &s_short_du_waveform; }
// Binary page navigation: each band starts one phase later. The target nibble
// holds the band, and the source nibble holds old/new binary flags. Index 15
// keeps existing white pixels and the border undriven.
static uint8_t (*s_reveal_data)[16][4];
static EpdWaveformPhases s_reveal_phases = {
    .phases=2*MY_FULL_DU_FRAMES+9, .phase_times=NULL, .luts=NULL,
};
static const EpdWaveformPhases* s_reveal_ranges[] = { &s_reveal_phases };
static const EpdWaveformMode s_reveal_mode = { .type=1, .temp_ranges=1, .range_data=s_reveal_ranges };
static const EpdWaveformMode* s_reveal_modes[] = { &s_reveal_mode };
static const EpdWaveform s_reveal_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_reveal_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_wipe_reveal(void) { return s_reveal_data ? &s_reveal_waveform : NULL; }
static uint8_t (*s_reveal_short_data)[16][4];
static EpdWaveformPhases s_reveal_short_phases = {
    .phases=25,.phase_times=NULL,.luts=NULL,
};
static const EpdWaveformPhases* s_reveal_short_ranges[] = { &s_reveal_short_phases };
static const EpdWaveformMode s_reveal_short_mode = { .type=1,.temp_ranges=1,.range_data=s_reveal_short_ranges };
static const EpdWaveformMode* s_reveal_short_modes[] = { &s_reveal_short_mode };
static const EpdWaveform s_reveal_short_waveform = {
    .num_modes=1,.num_temp_ranges=1,.mode_data=s_reveal_short_modes,.temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_wipe_reveal_short(void) { return s_reveal_short_data ? &s_reveal_short_waveform : NULL; }
const EpdWaveform* my_waveform_wipe_gl(int profile) {
    static uint8_t (*data[4])[16][4];
    static EpdWaveformPhases phases[4];
    static const EpdWaveformPhases* ranges[4];
    static EpdWaveformMode modes[4];
    static const EpdWaveformMode* mode_ptrs[4];
    static EpdWaveform waves[4];
    const int slot=profile>=0 && profile<4 ? profile : 0;
    if(waves[slot].num_modes) return &waves[slot];
    const EpdWaveform* source=slot==3 ? my_waveform_gray8() :
        (slot>=1 && my_waveform_fast() ? my_waveform_fast() : &MY_WAVEFORM);
    const int erase=slot>=2 ? 8 : MY_FULL_DU_FRAMES;
    const EpdWaveformPhases* gl=NULL;
    for(int m=0;m<source->num_modes;++m)
        if(source->mode_data[m]->type==5) gl=source->mode_data[m]->range_data[0];
    if(!gl || gl->phases>MY_FULL_GL16_FRAMES) return NULL;
    data[slot]=heap_caps_malloc((erase+gl->phases)*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!data[slot]) return NULL;
    memset(data[slot],0,(erase+gl->phases)*16*4);
    for(int to=0;to<16;++to) for(int old=0;old<16;++old) {
        for(int f=0;f<erase;++f)
            lut_or(data[slot],f,to,old,lut_get(slot>=2 ? s_short_du_data : my_du_25_0_data,f,15,old));
        // PAPER TARGET (r160). The ramp's own 15->15 cell is a HOLD (0 pulses), so for a
        // paper target the erase prefix above was the entire drive -- 8 white pushes, where
        // this panel's DU and GL16 columns spend 18 on the same black->paper transition.
        // There is no write afterwards to correct it, so the ink underneath a cleared glyph
        // simply survived; and because _full then copies front_fb->back_fb, every later drive
        // reads 15->15 and skips it (MODE_SKIP_UNCHANGED), which is why the residue froze the
        // instant it was made and outlived the next paint.
        // The composite therefore takes the ERASE end of the ramp (its from=0 column, the one
        // that starts from ink) so it finishes the erase the prefix began. Same phases, same
        // table slot, same allocation: ONLY the (to=15, old<15) cells change, from "hold" to
        // the white pushes the ramp already emits there. old==15 is deliberately excluded --
        // see the WIPE_PAPER_TICK switch above.
        const int src = (to == 15 && old != 15) ? 0 : 15;
        for(int f=0;f<gl->phases;++f) {
            const int action=(gl->luts[(f*16+to)*4+(src/4)] >> (6 - 2*(src%4))) & 3;
            lut_or(data[slot],erase+f,to,old,action);
        }
    }
    phases[slot]=(EpdWaveformPhases){.phases=erase+gl->phases,
        .phase_times=NULL,.luts=(const uint8_t*)data[slot]};
    ranges[slot]=&phases[slot];
    modes[slot]=(EpdWaveformMode){.type=5,.temp_ranges=1,.range_data=&ranges[slot]};
    mode_ptrs[slot]=&modes[slot];
    waves[slot]=(EpdWaveform){.num_modes=1,.num_temp_ranges=1,
        .mode_data=&mode_ptrs[slot],.temp_intervals=my_waveform_intervals};
    return &waves[slot];
}

// ERASE SWEEP (the real precondition of every from-white drive).
//
// epd_hl_update_screen_from_white() only DECLARES the baseline white: it
// memsets back_fb to 0xFF (highlevel.c:167) and then looks every pixel's
// waveform up as 15 -> target. That model is true on a freshly cleared panel
// and false on a surface that still carries the previous page -- exactly the
// case of a 16-level image / sleep card painted over text. Where the old page
// had ink and the new target is mid-grey the table then applies the short
// white -> light-grey move and the old ink is under-erased, which is the
// residue the operator photographed. Text pages never show it for a structural
// reason: their targets are black, and white -> black is the long sweep.
//
// This table makes the assumption true instead of assuming it: 20 DU frames
// taken from the to=white column (my_du_25_0_data[f][15][old]) -- the same
// erase prefix my_waveform_wipe_gl() uses above (:276-278) -- drive EVERY
// pixel to paper whatever state it was in. Mode type 2 so a MODE_GC16 request
// resolves to it through the normal mode lookup. Driven by the panel as a
// separate all-paper frame immediately before the content frame:
// 20 phases ~= 240 ms at the 12 ms floor, and only on 16-level GC16 paints.
static uint8_t (*s_erase_data)[16][4];
static EpdWaveformPhases s_erase_phases;
static const EpdWaveformPhases* s_erase_ranges[1];
static EpdWaveformMode s_erase_mode;
static const EpdWaveformMode* s_erase_modes[1];
static EpdWaveform s_erase_waveform;
const EpdWaveform* my_waveform_erase_white(void) {
    if (s_erase_waveform.num_modes) return &s_erase_waveform;
    const int erase = MY_FULL_DU_FRAMES;
    s_erase_data = heap_caps_malloc(erase*16*4, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_erase_data) return NULL;
    memset(s_erase_data, 0, erase*16*4);
    for (int to=0;to<16;++to) for (int old=0;old<16;++old)
        for (int f=0;f<erase;++f)
            lut_or(s_erase_data,f,to,old,lut_get(my_du_25_0_data,f,15,old));
    s_erase_phases=(EpdWaveformPhases){.phases=erase,.phase_times=NULL,
        .luts=(const uint8_t*)s_erase_data};
    s_erase_ranges[0]=&s_erase_phases;
    s_erase_mode=(EpdWaveformMode){.type=2,.temp_ranges=1,.range_data=s_erase_ranges};
    s_erase_modes[0]=&s_erase_mode;
    s_erase_waveform=(EpdWaveform){.num_modes=1,.num_temp_ranges=1,
        .mode_data=s_erase_modes,.temp_intervals=my_waveform_intervals};
    return &s_erase_waveform;
}
static void wipe_build(void) {
    // Waveform data is read before each scan into the INTERNAL conversion LUT.
    // Keep the large immutable source tables out of DMA-capable internal RAM.
    s_reveal_data=heap_caps_malloc((2*MY_FULL_DU_FRAMES+9)*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_reveal_short_data=heap_caps_malloc(25*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(s_reveal_data) memset(s_reveal_data,0,(2*MY_FULL_DU_FRAMES+9)*16*4);
    if(s_reveal_short_data) memset(s_reveal_short_data,0,25*16*4);
    s_reveal_phases.luts=(const uint8_t*)s_reveal_data;
    s_reveal_short_phases.luts=(const uint8_t*)s_reveal_short_data;
    for (int band=0;band<10;++band) for (int code=0;code<4;++code) {
        const int old=(code&1) ? 15 : 0;
        for (int f=0;f<MY_FULL_DU_FRAMES;++f) {
            if(s_reveal_data) lut_or(s_reveal_data,band+f,band,code,lut_get(my_du_25_0_data,f,15,old));
            if (s_reveal_data && (code&2)) lut_or(s_reveal_data,band+MY_FULL_DU_FRAMES+f,band,code,
                lut_get(my_du_25_0_data,f,0,15));
        }
        for(int f=0;f<8;++f) {
            if(s_reveal_short_data) lut_or(s_reveal_short_data,band+f,band,code,lut_get(s_short_du_data,f,15,old));
            if(s_reveal_short_data && (code&2)) lut_or(s_reveal_short_data,band+8+f,band,code,lut_get(s_short_du_data,f,0,15));
        }
    }
}
static void short_du_build(void) {
    for (int to=0;to<16;++to) for (int from=0;from<16;++from) {
        if (to==from) continue;
        const int endpoint=to<8 ? 0 : 15;
        const int action=endpoint==0 ? 1 : 2;
        const int frames=endpoint==0 ? 7 : 8;
        for (int f=0;f<frames;++f) lut_or(s_short_du_data,f,to,from,action);
    }
}
