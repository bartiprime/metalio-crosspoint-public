#pragma once
// SIMD1 host stub: the bench only calls esp_timer_get_time(), and the test
// provides its own monotonic counter for that symbol.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
