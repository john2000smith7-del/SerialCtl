#pragma once
// SerialCtl builds only the low-level tinfl decompressor from miniz 3.0.2.
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_MALLOC
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_HAS_64BIT_REGISTERS 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#include "miniz_tinfl.h"
