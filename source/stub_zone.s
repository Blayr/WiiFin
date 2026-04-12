/* stub_zone.s — embeds the CustomizeMii NAND-loader stub's own bytes at
 * 0x80804000–0x80831E3F.  This prevents the stub's code/stack from being
 * clobbered when IOS writes WiiFin's single data segment over that region:
 * the bytes written there are identical to what was already there, so the
 * stub's return address and stack remain intact after ReadContent returns.
 *
 * Generated from: tools/stub_zone_gen.py (reads the installed stub app)
 * Source: title/00010001/5749464e/content/00000001.app
 *   text[0] @ file 0x100   size 0x1D9C0  VA 0x80804000
 *   data[0] @ file 0x1DAC0 size 0x4140   VA 0x808219C0
 *   bss     @ (zeros)      size 0xC340   VA 0x80825B00
 */
    .section .stub_zone, "aw", @progbits
    .balign 4
    .global __stub_zone_start
__stub_zone_start:
    .incbin "tools/stub_zone.bin"
    .global __stub_zone_end
__stub_zone_end:
