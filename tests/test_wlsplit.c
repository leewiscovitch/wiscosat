#include "check.h"
#include "wlsplit.h"

void test_wlsplit(void)
{
    // within one flash sector: the whole of it, aligned or not
    CHECK(wlsplit_first(0, 4096, 4096) == 4096);
    CHECK(wlsplit_first(512, 512, 4096) == 512);
    CHECK(wlsplit_first(512, 3584, 4096) == 3584); // up to the boundary, not over it
    CHECK(wlsplit_first(8192 + 100, 1, 4096) == 1);
    // over a boundary: only up to it (a FAT cluster that starts one 512-byte sector into a flash sector)
    CHECK(wlsplit_first(512, 4096, 4096) == 3584);
    CHECK(wlsplit_first(4096 + 3584, 1024, 4096) == 512);
    CHECK(wlsplit_first(4095, 2, 4096) == 1);
    // aligned and longer than a sector: one sector at a time
    CHECK(wlsplit_first(4096, 10000, 4096) == 4096);
    // nothing to do stays nothing
    CHECK(wlsplit_first(512, 0, 4096) == 0);

    // walked to the end, every piece stays inside one flash sector and the pieces add up
    size_t addr = 97 * 512, left = 81000, pieces = 0;
    while (left) {
        size_t k = wlsplit_first(addr, left, 4096);
        CHECK(k > 0 && k <= left);
        CHECK(addr / 4096 == (addr + k - 1) / 4096);
        addr += k, left -= k, pieces++;
    }
    CHECK(addr == 97 * 512 + 81000);
    CHECK(pieces == 20);
}
