// Reads and writes cut at the flash sectors' boundaries (wlsplit.h).
#include "wlsplit.h"

size_t wlsplit_first(size_t addr, size_t size, size_t sector)
{
    size_t room = sector - addr % sector;
    return size < room ? size : room;
}
