// Host copy of SdFat's flag set for the NON-fcntl encoding (SdFatConfig.h USE_FCNTL_H == 0:
// AVR, and any non-ESP32/ARM target). Values are SdFat's own, from
// idf/components/extern/SdFat/src/common/FsApiConstants.h. Keep in step with that file.
#pragma once

#define O_RDONLY 0X00  ///< Open for reading only.
#define O_WRONLY 0X01  ///< Open for writing only.
#define O_RDWR 0X02    ///< Open for reading and writing.
#define O_AT_END 0X04  ///< Open at EOF.
#define O_APPEND 0X08  ///< Set append mode.
#define O_CREAT 0x10   ///< Create file if it does not exist.
#define O_TRUNC 0x20   ///< Truncate file to zero length.
#define O_EXCL 0x40    ///< Fail if the file exists.
#define O_SYNC 0x80    ///< Synchronized write I/O operations.

#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)  ///< Mask for access mode.
typedef unsigned char oflag_t;

#define O_READ O_RDONLY
#define O_WRITE O_WRONLY
