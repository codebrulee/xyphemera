/* fragmentedearth_native_space: native/space.hpp's space_reference on the device, a stage at a
   time over the block: down to half rate, the four diffusers, the loop's half-sum, its four
   lines, back to full rate. The lines are circular buffers (B = the words, L = their 16 KB),
   so each read or write is one access, stepping back a word a sample.
   R0 the words, R1 scratch (dl +0, dr +128 bytes; xl +256, xr +384 and the half-sum +512 are
   the routine's own, and dl and dr then hold what the loop gives each side), R2 the SpaceJob:
   left +0, right +4, at +8, dry +12, wet +16, gain +20.., damp +36.., last +52, +56. */
.section .text
.align 4
.global _fragmentedearth_native_space
.type _fragmentedearth_native_space, STT_FUNC
_fragmentedearth_native_space:
    [--SP] = (R7:4, P5:3);
    P3 = R0;                    /* the words */
    P4 = R1;                    /* scratch */
    P5 = R2;                    /* the job */
    B0 = R0;
    B1 = R0;
    B2 = R0;
    B3 = R0;
    P2 = 32;                    /* half-rate samples a block */

    /* Down to half rate: xl = dl + left[2i] / 4 + left[2i + 1] / 4, and xr likewise. */
    P0 = [P5];
    P1 = [P5 + 4];
    R0 = P4;
    R1 = 0 (X);
    R0 = R0 + R1;
    I0 = R0;
    R0 = P4;
    R1 = 128 (X);
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 256 (X);
    R0 = R0 + R1;
    I2 = R0;
    R0 = P4;
    R1 = 384 (X);
    R0 = R0 + R1;
    I3 = R0;
    LSETUP (.Ldown, .Ldown_end) LC0 = P2;
.Ldown:
    R0 = [P0++];
    R1 = [P0++];
    R0 >>>= 2;
    R1 >>>= 2;
    R2 = [I0++];
    R0 = R0 + R1;
    R0 = R0 + R2;
    [I2++] = R0;
    R0 = [P1++];
    R1 = [P1++];
    R0 >>>= 2;
    R1 >>>= 2;
    R2 = [I1++];
    R0 = R0 + R1;
    R0 = R0 + R2;
.Ldown_end:
    [I3++] = R0;

    /* The lines from here on are circular. */
    R0 = 16384 (X);
    L0 = R0;
    L1 = R0;
    L2 = R0;
    L3 = R0;
    R6 = 0 (X);
    R6.H = 0x1000;              /* the limit, 1 << 28 */
    R5 = -R6;
    /* Diffuser 0 on xl: d read 71 on, v = limit(in - d / 2 - d / 8) written, out d + v / 2 + v / 8. */
    R0 = [P5 + 8];              /* at */
    R1 = 71 (X);             /* line 0 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 0 (X);             /* line 0 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 256 (X);
    R0 = R0 + R1;
    P0 = R0;
    R1 = [I0--];                /* the first d (the loop reads each next one ahead) */
    LSETUP (.Ldiffuse0, .Ldiffuse0_end) LC0 = P2;
.Ldiffuse0:
    R2 = R1 >>> 1 || R0 = [P0] || NOP;
    R3 = R1 >>> 3;
    R2 = R2 + R3 (NS);          /* d / 2 + d / 8 */
    R0 = R0 - R2 (NS);
    R0 = MIN (R0, R6);
    R0 = MAX (R0, R5);
    R2 = R0 >>> 1 || [I1--] = R0 || NOP;
    R3 = R0 >>> 3;
    R2 = R2 + R3 (NS);
    R2 = R1 + R2 (NS) || R1 = [I0--] || NOP;
.Ldiffuse0_end:
    [P0++] = R2;

    /* Diffuser 1 on xl: d read 191 on, v = limit(in - d / 2 - d / 8) written, out d + v / 2 + v / 8. */
    R0 = [P5 + 8];              /* at */
    R1 = 263 (X);             /* line 1 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 72 (X);             /* line 1 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 256 (X);
    R0 = R0 + R1;
    P0 = R0;
    R1 = [I0--];                /* the first d (the loop reads each next one ahead) */
    LSETUP (.Ldiffuse1, .Ldiffuse1_end) LC0 = P2;
.Ldiffuse1:
    R2 = R1 >>> 1 || R0 = [P0] || NOP;
    R3 = R1 >>> 3;
    R2 = R2 + R3 (NS);          /* d / 2 + d / 8 */
    R0 = R0 - R2 (NS);
    R0 = MIN (R0, R6);
    R0 = MAX (R0, R5);
    R2 = R0 >>> 1 || [I1--] = R0 || NOP;
    R3 = R0 >>> 3;
    R2 = R2 + R3 (NS);
    R2 = R1 + R2 (NS) || R1 = [I0--] || NOP;
.Ldiffuse1_end:
    [P0++] = R2;

    /* Diffuser 2 on xr: d read 53 on, v = limit(in - d / 2 - d / 8) written, out d + v / 2 + v / 8. */
    R0 = [P5 + 8];              /* at */
    R1 = 317 (X);             /* line 2 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 264 (X);             /* line 2 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 384 (X);
    R0 = R0 + R1;
    P0 = R0;
    R1 = [I0--];                /* the first d (the loop reads each next one ahead) */
    LSETUP (.Ldiffuse2, .Ldiffuse2_end) LC0 = P2;
.Ldiffuse2:
    R2 = R1 >>> 1 || R0 = [P0] || NOP;
    R3 = R1 >>> 3;
    R2 = R2 + R3 (NS);          /* d / 2 + d / 8 */
    R0 = R0 - R2 (NS);
    R0 = MIN (R0, R6);
    R0 = MAX (R0, R5);
    R2 = R0 >>> 1 || [I1--] = R0 || NOP;
    R3 = R0 >>> 3;
    R2 = R2 + R3 (NS);
    R2 = R1 + R2 (NS) || R1 = [I0--] || NOP;
.Ldiffuse2_end:
    [P0++] = R2;

    /* Diffuser 3 on xr: d read 139 on, v = limit(in - d / 2 - d / 8) written, out d + v / 2 + v / 8. */
    R0 = [P5 + 8];              /* at */
    R1 = 457 (X);             /* line 3 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 318 (X);             /* line 3 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 384 (X);
    R0 = R0 + R1;
    P0 = R0;
    R1 = [I0--];                /* the first d (the loop reads each next one ahead) */
    LSETUP (.Ldiffuse3, .Ldiffuse3_end) LC0 = P2;
.Ldiffuse3:
    R2 = R1 >>> 1 || R0 = [P0] || NOP;
    R3 = R1 >>> 3;
    R2 = R2 + R3 (NS);          /* d / 2 + d / 8 */
    R0 = R0 - R2 (NS);
    R0 = MIN (R0, R6);
    R0 = MAX (R0, R5);
    R2 = R0 >>> 1 || [I1--] = R0 || NOP;
    R3 = R0 >>> 3;
    R2 = R2 + R3 (NS);
    R2 = R1 + R2 (NS) || R1 = [I0--] || NOP;
.Ldiffuse3_end:
    [P0++] = R2;

    /* The loop's half-sum: half of each of lines 4 to 7 as they come round. */
    R0 = [P5 + 8];              /* at */
    R1 = 981 (X);             /* line 4 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 1643 (X);             /* line 5 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 2405 (X);             /* line 6 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I2 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 3343 (X);             /* line 7 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I3 = R0;
    R0 = P4;
    R1 = 512 (X);
    R0 = R0 + R1;
    P0 = R0;
    LSETUP (.Lhalf, .Lhalf_end) LC0 = P2;
.Lhalf:
    MNOP || R0 = [I0--] || R1 = [I1--];
    R0 = R0 >>> 1 || R2 = [I2--] || NOP;
    R1 = R1 >>> 1 || R3 = [I3--] || NOP;
    R2 = R2 >>> 1;
    R3 = R3 >>> 1;
    R0 = R0 + R1 (NS);
    R2 = R2 + R3 (NS);
    R0 = R0 + R2 (NS);
.Lhalf_end:
    [P0++] = R0;

    /* The lines: d += (a - half - d) x 0.65; written limit(d x gain +- the input); a given to
       its side (4 and 7 the left, into dl; 5 and 6 the right, into dr). I2 and I3 are linear. */
    L2 = 0;
    L3 = 0;
    R7 = 0x3333 (Z);
    R7.H = 0x5333;              /* 0.65, Q31 */
    /* Line 4: plus xl, giving the left. */
    R0 = [P5 + 8];              /* at */
    R1 = 981 (X);             /* line 4 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 458 (X);             /* line 4 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 512 (X);
    R0 = R0 + R1;
    I2 = R0;
    R0 = P4;
    R1 = 0 (X);
    R0 = R0 + R1;
    I3 = R0;
    R0 = P4;
    R1 = 256 (X);
    R0 = R0 + R1;
    P0 = R0;
    R4 = [P5 + 36];          /* its damp */
    R3 = [P5 + 20];          /* its gain */
    LSETUP (.Lline4, .Lline4_end) LC0 = P2;
.Lline4:
    MNOP || R0 = [I0--] || R1 = [I2++];   /* a, half */
    R1 = R0 - R1 (NS) || R2 = [P0++] || NOP;
    R1 = R1 - R4 (NS);
    R1 = R1 * R7 (T);
    R4 = R4 + R1 (NS);          /* d */
    R1 = R4 * R3 (T);
    R1 = R1 + R2 (NS);
    R1 = MIN (R1, R6);
    R1 = MAX (R1, R5) || [I3++] = R0 || NOP;
.Lline4_end:
    [I1--] = R1;
    [P5 + 36] = R4;

    /* Line 5: plus xr, giving the right. */
    R0 = [P5 + 8];              /* at */
    R1 = 1643 (X);             /* line 5 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 982 (X);             /* line 5 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 512 (X);
    R0 = R0 + R1;
    I2 = R0;
    R0 = P4;
    R1 = 128 (X);
    R0 = R0 + R1;
    I3 = R0;
    R0 = P4;
    R1 = 384 (X);
    R0 = R0 + R1;
    P0 = R0;
    R4 = [P5 + 40];          /* its damp */
    R3 = [P5 + 24];          /* its gain */
    LSETUP (.Lline5, .Lline5_end) LC0 = P2;
.Lline5:
    MNOP || R0 = [I0--] || R1 = [I2++];   /* a, half */
    R1 = R0 - R1 (NS) || R2 = [P0++] || NOP;
    R1 = R1 - R4 (NS);
    R1 = R1 * R7 (T);
    R4 = R4 + R1 (NS);          /* d */
    R1 = R4 * R3 (T);
    R1 = R1 + R2 (NS);
    R1 = MIN (R1, R6);
    R1 = MAX (R1, R5) || [I3++] = R0 || NOP;
.Lline5_end:
    [I1--] = R1;
    [P5 + 40] = R4;

    /* Line 6: minus xl, giving the right. */
    R0 = [P5 + 8];              /* at */
    R1 = 2405 (X);             /* line 6 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 1644 (X);             /* line 6 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 512 (X);
    R0 = R0 + R1;
    I2 = R0;
    R0 = P4;
    R1 = 128 (X);
    R0 = R0 + R1;
    I3 = R0;
    R0 = P4;
    R1 = 256 (X);
    R0 = R0 + R1;
    P0 = R0;
    R4 = [P5 + 44];          /* its damp */
    R3 = [P5 + 28];          /* its gain */
    LSETUP (.Lline6, .Lline6_end) LC0 = P2;
.Lline6:
    MNOP || R0 = [I0--] || R1 = [I2++];   /* a, half */
    R1 = R0 - R1 (NS) || R2 = [P0++] || NOP;
    R1 = R1 - R4 (NS);
    R1 = R1 * R7 (T);
    R4 = R4 + R1 (NS);          /* d */
    R1 = R4 * R3 (T);
    R1 = R1 - R2 (NS);
    R1 = MIN (R1, R6) || R2 = [I3] || NOP;
    R1 = MAX (R1, R5);
    R2 = R2 + R0 (NS) || [I1--] = R1 || NOP;
.Lline6_end:
    [I3++] = R2;
    [P5 + 44] = R4;

    /* Line 7: minus xr, giving the left. */
    R0 = [P5 + 8];              /* at */
    R1 = 3343 (X);             /* line 7 read */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I0 = R0;
    R0 = [P5 + 8];              /* at */
    R1 = 2406 (X);             /* line 7 write */
    R0 = R0 + R1;
    R1 = 4095 (X);
    R0 = R0 & R1;
    R0 <<= 2;
    R1 = P3;
    R0 = R0 + R1;
    I1 = R0;
    R0 = P4;
    R1 = 512 (X);
    R0 = R0 + R1;
    I2 = R0;
    R0 = P4;
    R1 = 0 (X);
    R0 = R0 + R1;
    I3 = R0;
    R0 = P4;
    R1 = 384 (X);
    R0 = R0 + R1;
    P0 = R0;
    R4 = [P5 + 48];          /* its damp */
    R3 = [P5 + 32];          /* its gain */
    LSETUP (.Lline7, .Lline7_end) LC0 = P2;
.Lline7:
    MNOP || R0 = [I0--] || R1 = [I2++];   /* a, half */
    R1 = R0 - R1 (NS) || R2 = [P0++] || NOP;
    R1 = R1 - R4 (NS);
    R1 = R1 * R7 (T);
    R4 = R4 + R1 (NS);          /* d */
    R1 = R4 * R3 (T);
    R1 = R1 - R2 (NS);
    R1 = MIN (R1, R6) || R2 = [I3] || NOP;
    R1 = MAX (R1, R5);
    R2 = R2 + R0 (NS) || [I1--] = R1 || NOP;
.Lline7_end:
    [I3++] = R2;
    [P5 + 48] = R4;

    /* Back to full rate: wl = (a4 + a7) x wet; left[2i] = left[2i] x dry + last / 2 + wl / 2,
       left[2i + 1] = left[2i + 1] x dry + wl; the right likewise. All linear again. */
    L0 = 0;
    L1 = 0;
    R0 = P4;
    R1 = 0 (X);
    R0 = R0 + R1;
    I0 = R0;
    R0 = P4;
    R1 = 128 (X);
    R0 = R0 + R1;
    I1 = R0;
    P0 = [P5];
    P1 = [P5 + 4];
    R6 = [P5 + 12];             /* dry */
    R7 = [P5 + 16];             /* wet */
    R4 = [P5 + 52];             /* last left */
    R5 = [P5 + 56];             /* last right */
    LSETUP (.Lup, .Lup_end) LC0 = P2;
.Lup:
    MNOP || R0 = [I0++] || R1 = [P0];
    R0 = R0 * R7 (T) || R3 = [P0 + 4] || NOP;
    R1 = R1 * R6 (T);
    R2 = R4 >>> 1;
    R1 = R1 + R2 (NS);
    R2 = R0 >>> 1;
    R1 = R1 + R2 (NS);
    R3 = R3 * R6 (T) || [P0++] = R1 || NOP;
    R3 = R3 + R0 (NS);
    R4 = R0;
    [P0++] = R3;
    MNOP || R0 = [I1++] || R1 = [P1];
    R0 = R0 * R7 (T) || R3 = [P1 + 4] || NOP;
    R1 = R1 * R6 (T);
    R2 = R5 >>> 1;
    R1 = R1 + R2 (NS);
    R2 = R0 >>> 1;
    R1 = R1 + R2 (NS);
    R3 = R3 * R6 (T) || [P1++] = R1 || NOP;
    R3 = R3 + R0 (NS);
    R5 = R0;
.Lup_end:
    [P1++] = R3;
    [P5 + 52] = R4;
    [P5 + 56] = R5;

    /* no circular buffer left set */
    L0 = 0;
    L1 = 0;
    L2 = 0;
    L3 = 0;
    (R7:4, P5:3) = [SP++];
    RTS;
.size _fragmentedearth_native_space, .-_fragmentedearth_native_space
