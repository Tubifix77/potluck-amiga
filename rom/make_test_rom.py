#!/usr/bin/env python3
"""Make potluck-amiga's own test ROM: a 68000 program that drives the Amiga's chips by itself, no Kickstart.

    python rom/make_test_rom.py rom/testrom.bin

Written for M27 (an emulator beside Potluck): the experiment asks what streaming an emulator's frames costs the
radio, so the frames have to move, not to come from Commodore's software. This program owes nothing to Kickstart:
it switches the boot overlay off, draws diagonal stripes into one bitplane, builds a copper list with 52 colour bars,
and then every frame scrolls the bitplane down a line and shifts the bars' colours one step -- every visible line
changes in every frame, the worst case for a stream that sends only changed lines.

The ROM is small (the board and the PC harness pad it to Kickstart's 256 KB, mapped at 0xF80000 and mirrored at
0xFC0000; at reset the overlay shows it at 0, where the 68000 takes its stack pointer and start address).

The instructions are encoded here by hand, each with the 68000's encoding written out beside it (Motorola's
M68000 Programmer's Reference Manual, section 4/8): there is no 68000 assembler on the bench PC.
"""

from __future__ import annotations

import struct
import sys

ROM_BASE = 0xFC0000      # where the code runs once the overlay is gone (the mirror Kickstart 1.3 uses)
PLANE = 0x10000          # chip RAM: one bitplane, 512 rows of 40 bytes (scrolled through 256 of them)
COPPER = 0x20000         # chip RAM: the copper list, copied there from this ROM
BARS = 52                # colour bars, 4 lines apart from line 44 (the copper's WAIT holds 8 bits of line)
FIRST_BAR_LINE = 44
WAIT_LINE = 20           # the CPU does a frame's work once the beam passes this line (top border)


class Asm:
    def __init__(self, base: int) -> None:
        self.base = base
        self.out = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str, str]] = []  # (offset of the displacement, label, kind)

    @property
    def pc(self) -> int:
        return self.base + len(self.out)

    def w(self, *words: int) -> None:
        for v in words:
            self.out += struct.pack(">H", v & 0xFFFF)

    def l(self, *longs: int) -> None:
        for v in longs:
            self.out += struct.pack(">I", v & 0xFFFFFFFF)

    def label(self, name: str) -> None:
        self.labels[name] = self.pc

    def bcc_s(self, cond: int, name: str) -> None:
        """Bcc.S: 0110 cccc dddddddd, displacement from the opcode's address + 2."""
        self.fixups.append((len(self.out), name, "s"))
        self.w(0x6000 | (cond << 8))

    def dbra(self, dn: int, name: str) -> None:
        """DBF Dn: 0101 0001 1100 1rrr, then a 16-bit displacement from its own address."""
        self.w(0x51C8 | dn)
        self.fixups.append((len(self.out), name, "w"))
        self.w(0)

    def bra_w(self, name: str) -> None:
        """BRA.W: 0110 0000 0000 0000, then a 16-bit displacement from its own address."""
        self.w(0x6000)
        self.fixups.append((len(self.out), name, "w"))
        self.w(0)

    def addr_of(self, name: str) -> int:
        return self.labels[name]

    def link(self) -> bytes:
        for off, name, kind in self.fixups:
            target = self.labels[name]
            if kind == "s":
                disp = target - (self.base + off + 2)
                assert -128 <= disp <= 127 and disp != 0, (name, disp)
                self.out[off + 1] = disp & 0xFF
            else:
                disp = target - (self.base + off)
                assert -32768 <= disp <= 32767, (name, disp)
                self.out[off:off + 2] = struct.pack(">h", disp)
        return bytes(self.out)


NE, EQ = 0x6, 0x7


def copper_list() -> list[int]:
    words = [
        0x00E0, PLANE >> 16,      # BPL1PTH
        0x00E2, PLANE & 0xFFFF,   # BPL1PTL -- the CPU writes the scroll here (word 3)
        0x008E, 0x2C81,           # DIWSTRT
        0x0090, 0x2CC1,           # DIWSTOP
        0x0092, 0x0038,           # DDFSTRT
        0x0094, 0x00D0,           # DDFSTOP
        0x0100, 0x1200,           # BPLCON0: one bitplane, colour
        0x0102, 0x0000,           # BPLCON1
        0x0108, 0x0000,           # BPL1MOD
        0x0182, 0x0FFF,           # COLOR01: white stripes
    ]
    for k in range(BARS):
        line = FIRST_BAR_LINE + 4 * k
        words += [(line << 8) | 0x07, 0xFFFE,   # WAIT for the line
                  0x0180, 0x0000]               # MOVE COLOR00 -- the CPU writes the colour here
    words += [0xFFFF, 0xFFFE]                   # the end
    return words


COLOURS = [0xF00, 0xF60, 0xFA0, 0xFF0, 0xAF0, 0x0F0, 0x0FA, 0x0FF,
           0x0AF, 0x00F, 0x60F, 0xA0F, 0xF0F, 0xF0A, 0x800, 0x000]


def build() -> bytes:
    cop = copper_list()
    bplptl_value = COPPER + 3 * 2                      # the BPL1PTL MOVE's value word
    first_bar_value = COPPER + (20 + 3) * 2            # bar 0's COLOR00 value word (after the 10 MOVEs)

    a = Asm(ROM_BASE)
    # 0x00: the reset vectors, read through the overlay: stack pointer, start address
    a.l(0x00080000, 0)
    a.label("start")
    a.w(0x46FC, 0x2700)                                # move.w #$2700,sr        (supervisor, interrupts off)
    a.w(0x4DF9); a.l(0xDFF000)                         # lea $DFF000,a6          0100 110 111 111001
    a.w(0x3D7C, 0x7FFF, 0x009A)                        # move.w #$7FFF,$9A(a6)   INTENA: all off
    a.w(0x3D7C, 0x7FFF, 0x0096)                        # move.w #$7FFF,$96(a6)   DMACON: all off
    a.w(0x13FC, 0x0003); a.l(0xBFE201)                 # move.b #3,$BFE201       CIA-A DDRA: OVL and LED out
    a.w(0x13FC, 0x0002); a.l(0xBFE001)                 # move.b #2,$BFE001       OVL=0: chip RAM at 0
    a.w(0x4FF9); a.l(0x00080000)                       # lea $80000,sp

    # the bitplane: 512 rows of ten longs, each row the last rotated one bit -> diagonal stripes
    a.w(0x41F9); a.l(PLANE)                            # lea PLANE,a0
    a.w(0x223C); a.l(0xFF00FF00)                       # move.l #$FF00FF00,d1    0010 001 000 111100
    a.w(0x343C, 511)                                   # move.w #511,d2
    a.label("row")
    a.w(0x7009)                                        # moveq #9,d0
    a.label("long")
    a.w(0x20C1)                                        # move.l d1,(a0)+         0010 000 011 000001
    a.dbra(0, "long")
    a.w(0xE399)                                        # rol.l #1,d1             1110 001 1 10 0 11 001
    a.dbra(2, "row")

    # the copper list: copied from this ROM into chip RAM, where the copper reads
    a.w(0x41F9); a.l(0)                                # lea copper_src,a0       (patched below)
    src_fix = len(a.out) - 4
    a.w(0x43F9); a.l(COPPER)                           # lea COPPER,a1
    a.w(0x303C, len(cop) - 1)                          # move.w #n-1,d0
    a.label("copy")
    a.w(0x32D8)                                        # move.w (a0)+,(a1)+      0011 001 011 011000
    a.dbra(0, "copy")
    a.w(0x2D7C); a.l(COPPER); a.w(0x0080)              # move.l #COPPER,$80(a6)  COP1LC
    a.w(0x3D7C, 0x0000, 0x0088)                        # move.w #0,$88(a6)       COPJMP1
    a.w(0x3D7C, 0x8380, 0x0096)                        # move.w #$8380,$96(a6)   DMACON: DMAEN, BPLEN, COPEN
    a.w(0x7E00)                                        # moveq #0,d7             the frame counter

    a.label("frame")                                   # wait for the beam to reach WAIT_LINE
    a.w(0x202E, 0x0004)                                # move.l 4(a6),d0         VPOSR:VHPOSR
    a.w(0xE088)                                        # lsr.l #8,d0             1110 000 0 10 0 01 000
    a.w(0x0240, 0x01FF)                                # andi.w #$1FF,d0         -> the beam's line
    a.w(0x0C40, WAIT_LINE)                             # cmpi.w #WAIT_LINE,d0
    a.bcc_s(NE, "frame")

    a.w(0x5247)                                        # addq.w #1,d7            0101 001 0 01 000 111
    a.w(0x3007)                                        # move.w d7,d0
    a.w(0x0240, 0x00FF)                                # andi.w #255,d0
    a.w(0xC0FC, 40)                                    # mulu.w #40,d0           1100 000 011 111100
    a.w(0x33C0); a.l(bplptl_value)                     # move.w d0,BPL1PTL value -> the stripes scroll a line
    a.w(0x41F9); a.l(first_bar_value)                  # lea bar 0's value,a0
    a.w(0x43F9); a.l(0)                                # lea colours,a1          (patched below)
    col_fix = len(a.out) - 4
    a.w(0x7200 | (BARS - 1))                           # moveq #BARS-1,d1
    a.w(0x3407)                                        # move.w d7,d2
    a.label("bar")
    a.w(0x3602)                                        # move.w d2,d3
    a.w(0x0243, 0x000F)                                # andi.w #15,d3
    a.w(0xD643)                                        # add.w d3,d3             1101 011 001 000 011
    a.w(0x30B1, 0x3000)                                # move.w 0(a1,d3.w),(a0)  0011 000 010 110001 + d3.w
    a.w(0x5088)                                        # addq.l #8,a0            0101 000 0 10 001 000
    a.w(0x5242)                                        # addq.w #1,d2
    a.dbra(1, "bar")

    a.label("leave")                                   # wait for the beam to leave WAIT_LINE: once a frame
    a.w(0x202E, 0x0004)                                # move.l 4(a6),d0
    a.w(0xE088)                                        # lsr.l #8,d0
    a.w(0x0240, 0x01FF)                                # andi.w #$1FF,d0
    a.w(0x0C40, WAIT_LINE)                             # cmpi.w #WAIT_LINE,d0
    a.bcc_s(EQ, "leave")
    a.bra_w("frame")

    a.label("colours")
    a.w(*COLOURS)
    a.label("copper_src")
    a.w(*cop)

    rom = bytearray(a.link())
    struct.pack_into(">I", rom, 4, a.addr_of("start"))
    struct.pack_into(">I", rom, src_fix, a.addr_of("copper_src"))
    struct.pack_into(">I", rom, col_fix, a.addr_of("colours"))
    return bytes(rom)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2])
        return 2
    rom = build()
    with open(sys.argv[1], "wb") as f:
        f.write(rom)
    print(f"{sys.argv[1]}: {len(rom)} bytes (padded to 262,144 where it is loaded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
