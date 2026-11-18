/**
 * Report PSRAM state through the one console that actually reaches us.
 *
 * Why our own code does this instead of reading IDF's logs:
 *
 *   IDF's esp_log writes to the console that was selected when the library was
 *   COMPILED, and on this target IDF is linked from prebuilt Arduino archives
 *   (framework-arduinoespressif32-libs). Setting CONFIG_ESP_CONSOLE_* in our
 *   sdkconfig therefore cannot move IDF's log output -- those messages go to
 *   UART0, which is the CH340 that Windows will not open. Setting
 *   CONFIG_LOG_DEFAULT_LEVEL_INFO had no visible effect for the same reason.
 *
 *   esp_rom_printf, by contrast, goes out on ROM code, and the ROM mirrors its
 *   output to USB-Serial-JTAG -- which is why our own [DBG] lines appear on
 *   COM4 while IDF's never have.
 *
 * So this asks the heap directly, early in setup(), and prints the answer over
 * the path that works. It is the measurement that distinguishes the two
 * remaining hypotheses:
 *
 *   total == 0  -> PSRAM was never registered with the allocator. The fault at
 *                  0x50000020 is then an initialisation/mapping failure, not a
 *                  PMA permission problem.
 *   total > 0   -> PSRAM is registered and something else is wrong: the PMA
 *                  attribute table, or an early store before init.
 */

#include <stdint.h>
#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

/* The CSR number must be a literal, so each access is spelled out. */
static inline uint32_t pma7_cfg(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, 0xbc7" : "=r"(v));
    return v;
}

static inline uint32_t pma7_addr(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, 0xbd7" : "=r"(v));
    return v;
}

static inline uint32_t pma15_cfg(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, 0xbcf" : "=r"(v));
    return v;
}

static inline uint32_t pma15_addr(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, 0xbdf" : "=r"(v));
    return v;
}

/* Point PMA entry 7 at the whole external RAM window, R+W+X, locked. These are
 * the exact values measured from a from-source IDF boot that works (probe/):
 * cfg 0xe000001d, addr 0x147fffff. Flash is entry 15, not entry 7. */
static inline void pma7_map_psram(void)
{
    const uint32_t addr = 0x147fffffu; /* NAPOT, 0x50000000..0x54000000 */
    const uint32_t cfg  = 0xe000001du; /* NAPOT|LOCK|EN|R|W|X */
    __asm__ volatile("csrw 0xbd7, %0" ::"r"(addr) : "memory");
    __asm__ volatile("csrw 0xbc7, %0" ::"r"(cfg) : "memory");
}

/* Re-establishing the PSRAM mapping is the step that kills the console
 * (builds 53 and 54 both died before USB came up), so it is compiled out by
 * default and turned on deliberately in a build of its own. Reporting the PMA
 * state costs nothing and is what the next build is for. */
/* Now enabled: build55 measured entry 7 as cfg=08000000 addr=00000000 in this
 * build -- nothing mapped -- while a from-source IDF boot has cfg=e000001d
 * addr=147fffff. That single unmapped entry is the whole PSRAM fault. */
#define MCCROSSY_PMA_WRITE 1

extern "C" void mccrossy_psram_diag(void)
{
    esp_rom_printf("\n[pma] before: e7 cfg=%08x addr=%08x | e15 cfg=%08x addr=%08x\n",
                   (unsigned)pma7_cfg(), (unsigned)pma7_addr(),
                   (unsigned)pma15_cfg(), (unsigned)pma15_addr());

#if MCCROSSY_PMA_WRITE
    pma7_map_psram();

    esp_rom_printf("[pma] after : e7 cfg=%08x addr=%08x | e15 cfg=%08x addr=%08x\n",
                   (unsigned)pma7_cfg(), (unsigned)pma7_addr(),
                   (unsigned)pma15_cfg(), (unsigned)pma15_addr());
#endif

    const size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    /* Also report internal RAM so the line is unambiguous about which pool is
     * which -- an empty PSRAM pool next to a healthy internal one is exactly
     * the signature we are looking for. */
    const size_t internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    esp_rom_printf("\n[psram] SPIRAM total=%u free=%u largest=%u | INTERNAL total=%u\n",
                   (unsigned)total, (unsigned)freeBytes, (unsigned)largest,
                   (unsigned)internal);

    if (total == 0) {
        esp_rom_printf("[psram] PSRAM IS NOT REGISTERED -- init/mapping failed, "
                       "not a PMA permission problem\n");
        return;
    }

    /* The allocation and store test only make sense once the PMA mapping is in
     * place: without it the first store into external RAM faults at
     * 0x50000020. Wrapped behind the same switch so this build can boot. */
#if MCCROSSY_PMA_WRITE
    void *p = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    esp_rom_printf("[psram] malloc(4096, SPIRAM) -> %p\n", p);
    if (!p) {
        esp_rom_printf("[psram] PSRAM allocation FAILED\n");
        return;
    }
    volatile uint32_t *w = (volatile uint32_t *)p;
    w[0] = 0xA5A5A5A5u;
    w[1] = 0x5A5A5A5Au;
    esp_rom_printf("[psram] write/readback %08x %08x -> %s\n",
                   (unsigned)w[0], (unsigned)w[1],
                   (w[0] == 0xA5A5A5A5u && w[1] == 0x5A5A5A5Au) ? "OK" : "MISMATCH");

    /* And touch the exact address from the fault dump, if it is inside the
     * region we were granted. */
    volatile uint32_t *base = (volatile uint32_t *)0x50000020u;
    base[0] = 0x12345678u;
    esp_rom_printf("[psram] store to 0x50000020 OK, readback %08x\n",
                   (unsigned)base[0]);
    heap_caps_free(p);
    esp_rom_printf("[psram] PSRAM TEST PASSED\n");
#endif
}
