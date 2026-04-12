/*
 * crt0_pre.S — Entry point, jumps to libogc's __app_start.
 *
 * .sbss and .bss are now contiguous in the linker script, so crt0's
 * memset(__sbss_start, 0, __bss_end - __sbss_start) covers both.
 * No manual zeroing needed here.
 */
    .section .crt0_pre, "ax"
    .global __wiifin_start

__wiifin_start:
    b       __app_start
