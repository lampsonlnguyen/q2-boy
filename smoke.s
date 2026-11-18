// SPDX-License-Identifier: GPL-2.0-or-later
.syntax unified
.arm
.section .text
.global _start
_start:
    b entry
    .org 0xa0
    .ascii "Q2GBATEST   "
    .ascii "QTST"
    .ascii "00"
    .byte 0x96, 0, 0
    .org 0xc0
entry:
    ldr r0, =0x04000000
    ldr r1, =0x403
    strh r1, [r0]
    ldr r2, =0x06000000
    ldr r3, =38400
    ldr r4, =0x001f
fill:
    strh r4, [r2], #2
    subs r3, r3, #1
    bne fill
again:
    add r5, r5, #1
    b again
