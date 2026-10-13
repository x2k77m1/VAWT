| SPDX-License-Identifier: GPL-2.0-or-later
| VAWT: put our own parameters 182/183 on SYN1 page 1 knobs D/A.
| ColdFire V4; assemble with -mcpu=54455, like core-dn1.
|
| Knob D's stock parameter (110) only spans raw 0-2, so a -2..+2 octave
| cannot live on it. This is called from the SYN1 page constructor in
| place of `move.l d1,$PAGE_D_REC.l` (6 bytes: a jsr fits exactly), which
| wrote stock id 110: d1 must read 182 at the store. PAGE_D_REC comes
| from mod.json's defsym per OS version. Keeps every register but d1
| (which the displaced instruction wrote).
        .section .run, "ax"
        .globl  vawt_knob182
vawt_knob182:
        move.l  #182, %d1
        move.l  %d1, PAGE_D_REC
        rts
| Knob A's stock parameter (107) spans raw 0-7, one more than the seven
| VAWT waves (0-6), leaving a dead silent slot at the top. Called from
| the same constructor in place of `move.l d1,$PAGE_A_REC.l` (same shape:
| d1 holds the stock id 107, confirmed by the moveq #107 just before the
| site on both OS versions). Unlike knob D this one checks the track's
| machine first, so FM tracks keep stock 107: d1 reads 183 on VAWT, 107
| elsewhere. Condition codes are saved/restored around the compares
| (the displaced move.l left CCR untouched).
| PAGE_A_REC comes from mod.json's defsym per OS version.
        .globl  vawt_knob183
vawt_knob183:
        move.l  %d0, -(%sp)
        move.l  %a0, -(%sp)
        move.w  %ccr, %d1             | d1's entry value (stock 107) is
        move.l  %d1, -(%sp)           | discarded anyway; CCR rides on stack
        move.l  FW_ACTIVE_TRACK, %d0
        movea.l FW_KIT, %a0
        cmpa.l  #0, %a0
        beq.s   1f                     | no kit yet: stock id
        mulu.w  #326, %d0
        adda.l  %d0, %a0
        move.w  0x2c(%a0), %d0
        cmpi.w  #0x0A00, %d0          | VAWT machine id << 8?
        bne.s   1f
        move.l  #183, %d1
        bra.s   2f
1:      move.l  #107, %d1
2:      move.l  %d1, PAGE_A_REC
        move.l  (%sp)+, %d1
        move.w  %d1, %ccr
        move.l  (%sp)+, %a0
        move.l  (%sp)+, %d0
        rts
