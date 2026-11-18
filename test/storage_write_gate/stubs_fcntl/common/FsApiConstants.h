// Host copy of SdFat's flag set for the FCNTL encoding -- the one this board actually builds
// with (SdFatConfig.h: `#elif defined(ESP32) -> USE_FCNTL_H 1`, so FsApiConstants.h includes
// <fcntl.h> and oflag_t is int). This stub takes the values from the HOST libc's fcntl.h, which
// is how the ESP32 newlib set is shaped: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_ACCMODE 3, O_APPEND
// 0x8, O_CREAT 0x40/0x200, O_TRUNC 0x200/0x400, O_AT_END == O_NONBLOCK.
#pragma once

#include <fcntl.h>

// SdFat defines O_AT_END as O_NONBLOCK under USE_FCNTL_H. glibc hides O_NONBLOCK under C++20's
// strict feature set, so the value is spelled out: it is host glibc's O_NONBLOCK (0x800). The
// ESP32 newlib value (0x4000) differs, which is the point -- O_AT_END only positions the handle
// at EOF, so its value must not matter to the predicate, and the two encodings prove that.
#ifndef O_AT_END
#define O_AT_END 0x800  ///< Open at EOF (== O_NONBLOCK on this host libc).
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif

typedef int oflag_t;

#define O_READ O_RDONLY
#define O_WRITE O_WRONLY
