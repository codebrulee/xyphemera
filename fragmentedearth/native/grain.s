/* fragmentedearth_native_grain: native/grain.hpp's grain_reference on the device.
   R0 the recording (where the window begins), R1 out_left, R2 the GrainJob:
   position +0, rate +4, count +8, left +12, step_left +16, right +20, step_right +24,
   out_right +28, scratch +32. Two passes: read and interpolate into scratch, then mix. */
.section .text
.align 4
.global _fragmentedearth_native_grain
.type _fragmentedearth_native_grain, STT_FUNC
_fragmentedearth_native_grain:
    [--SP] = (R7:4, P5:3);
    P5 = R0;                    /* the recording */
    P4 = R1;                    /* out_left */
    P3 = R2;                    /* the job */
    R0 = [P3 + 8];              /* count */
    CC = R0 == 0;
    IF CC JUMP .Ldone;          /* a loop count of 0 would still run once */
    P1 = R0;
    R4 = [P3];                  /* position */
    R5 = [P3 + 4];              /* rate */
    P2 = [P3 + 32];             /* scratch */
    /* v = (a + (b - a) x fraction) << 2, a and b the two recorded samples about position,
       each decoded: (c >> 4) << ((c & 15) + 6). */
    LSETUP (.Lread, .Lread_end) LC0 = P1;
.Lread:
    R0 = R4 >> 14;
    P0 = R0;
    P0 = P5 + (P0 << 1);
    R6 = R4 << 18 || R1 = W[P0] (X) || NOP;
    R6 = R6 >> 1 || R2 = W[P0 + 2] (X) || NOP;     /* the fraction, Q31 */
    R3 = R1 << 28;
    R7 = R2 << 28;
    R3 = R3 >> 28;
    R7 = R7 >> 28;
    R3 += 6;
    R7 += 6;
    R1 = R1 >>> 4;
    R2 = R2 >>> 4;
    R1 = ASHIFT R1 BY R3.L;     /* a */
    R2 = ASHIFT R2 BY R7.L;     /* b */
    R2 = R2 - R1 (NS);
    R2 = R2 * R6 (T);
    R2 = R2 + R1 (NS);
    R2 <<= 2;
.Lread_end:
    R4 = R4 + R5 (NS) || [P2++] = R2 || NOP;
    [P3] = R4;                  /* where it ends */
    /* out += (v x gain) >> 5 each side, the gains stepping on a sample. */
    P2 = [P3 + 32];
    R0 = P4;
    I0 = R0;                    /* out_left */
    R0 = [P3 + 28];
    I1 = R0;                    /* out_right */
    R4 = [P3 + 12];             /* left */
    R5 = [P3 + 16];
    R6 = [P3 + 20];             /* right */
    R7 = [P3 + 24];
    LSETUP (.Lmix, .Lmix_end) LC0 = P1;
.Lmix:
    MNOP || R0 = [P2++] || R2 = [I0];
    R1 = R0 * R4 (T) || R3 = [I1] || NOP;
    R1 = R1 >>> 5;
    R2 = R2 + R1 (NS);
    R1 = R0 * R6 (T) || [I0++] = R2 || NOP;
    R1 = R1 >>> 5;
    R3 = R3 + R1 (NS);
    R4 = R4 + R5 (NS) || [I1++] = R3 || NOP;
.Lmix_end:
    R6 = R6 + R7;
.Ldone:
    (R7:4, P5:3) = [SP++];
    RTS;
.size _fragmentedearth_native_grain, .-_fragmentedearth_native_grain
