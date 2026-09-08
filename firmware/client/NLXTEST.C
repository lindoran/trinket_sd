#include <stdio.h>

/* Mirrors the exact DWORD shift/multiply patterns sdtest.c's own
 * N_LX-calling functions use (sd2_write_sectors, sd_cmd,
 * sd_arg_for_lba, rd_u32, fat16_mount, fat16_cluster_lba,
 * fat16_next_cluster, cmd_bench, cmd_fastbench) -- same operand
 * types/widths, same -ms small model build, run from an ordinary
 * main()-rooted call stack (never a driver's private stack). Proves
 * these execute and return correct values under BC31, not just that
 * they compile. */

int main(void) {
    unsigned count = 37;
    unsigned long arg = 0x12345678ul;
    unsigned long lba = 0xABCDEFul;
    unsigned char raw[4] = {0x78, 0x56, 0x34, 0x12};
    unsigned cluster = 12345;
    unsigned sectors_per_cluster = 8;
    unsigned long data_start = 0x1000ul;
    int ok = 1;

    /* sd2_write_sectors: (unsigned long)count * 512ul */
    {
        unsigned long total = (unsigned long)count * 512ul;
        printf("count*512ul: %lu (expect 18944)\n", total);
        if (total != 18944ul) ok = 0;
    }

    /* sd_cmd/sd2_*: arg >> 24 / >> 16 / >> 8, byte-at-a-time */
    {
        unsigned char b0 = (unsigned char)(arg);
        unsigned char b1 = (unsigned char)(arg >> 8);
        unsigned char b2 = (unsigned char)(arg >> 16);
        unsigned char b3 = (unsigned char)(arg >> 24);
        printf("arg bytes: %02X %02X %02X %02X (expect 78 56 34 12)\n", b0, b1, b2, b3);
        if (b0 != 0x78 || b1 != 0x56 || b2 != 0x34 || b3 != 0x12) ok = 0;
    }

    /* sd_arg_for_lba (block_addressed==0 path): lba * 512ul */
    {
        unsigned long byte_addr = lba * 512ul;
        printf("lba*512ul truncated: 0x%08lX (expect 0x579BDE00)\n", byte_addr);
        /* 0xABCDEF * 512 = 0x1579BDE00 -> truncates to 0x579BDE00 in 32-bit */
        if (byte_addr != 0x579BDE00ul) ok = 0;
    }

    /* rd_u32: byte[0] | byte[1]<<8 | byte[2]<<16 | byte[3]<<24 */
    {
        unsigned long v = (unsigned long)raw[0] | ((unsigned long)raw[1] << 8)
                         | ((unsigned long)raw[2] << 16) | ((unsigned long)raw[3] << 24);
        printf("rd_u32: 0x%08lX (expect 0x12345678)\n", v);
        if (v != 0x12345678ul) ok = 0;
    }

    /* fat16_cluster_lba: data_start + (cluster-2)*sectors_per_cluster */
    {
        unsigned long r = data_start + (unsigned long)(cluster - 2) * sectors_per_cluster;
        printf("cluster_lba: %lu (expect 102840)\n", r);
        if (r != 102840ul) ok = 0;
    }

    /* fat16_next_cluster: cluster*2, then /512 and %512 */
    {
        unsigned long fat_byte_off = (unsigned long)cluster * 2;
        unsigned long fat_sector = 100ul + fat_byte_off / 512;
        unsigned in_off = (unsigned)(fat_byte_off % 512);
        printf("fat_sector: %lu in_off: %u (expect 148 114)\n", fat_sector, in_off);
        if (fat_sector != 148ul || in_off != 114u) ok = 0;
    }

    /* cmd_bench/cmd_fastbench: (elapsed_ticks * 54925ul) / 1000ul */
    {
        unsigned long elapsed_ticks = 913;
        unsigned long ms = (elapsed_ticks * 54925ul) / 1000ul;
        printf("ticks->ms: %lu (expect 50146)\n", ms);
        if (ms != 50146ul) ok = 0;
    }

    printf(ok ? "ALL CHECKS PASSED\n" : "MISMATCH -- SOMETHING WRONG\n");
    return ok ? 0 : 1;
}
