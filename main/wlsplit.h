// A read or write on the wear-levelled seed partition, cut at the flash sectors' boundaries. Pure, tested on Linux.
// Why: wlfix.c.
#pragma once
#include <stddef.h>

// how many of the size bytes at addr may go in one call: up to the end of addr's flash sector (of sector bytes)
size_t wlsplit_first(size_t addr, size_t size, size_t sector);
