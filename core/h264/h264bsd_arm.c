/*
 * MP4 Player 9300: ARM assembler versions of h264bsd's hot loops (see h264bsd_arm.h).
 * Written for ARMv4T (ARM9, GCC 2.9 / old GNU as syntax): no CLZ, no saturating or SIMD
 * instructions, no PLD. Each routine is checked against the C original in the ARM emulator
 * (tools/armemu: run.py ktest) and with whole streams (tools/armemu/h264check.sh).
 *
 * Register use follows the ARM procedure call standard: r0-r3, r12 scratch, r4-r11 saved,
 * return with bx lr (interworking).
 */
#include "h264bsd_arm.h"

#ifdef H264_ARM_ASM

/* clip((x + 16) >> 5) for the 6-tap filters, indexed by x >> 4 (x from -2550 to 10710):
 * ((x + 16) >> 5) == ((x >> 4) + 1) >> 1, so entry i (from -160) is clip((i + 1) >> 1) */
const u8 h264arm_clip6tbl[830] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20,
    20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30,
    30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40,
    40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48, 48, 49, 49, 50,
    50, 51, 51, 52, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57, 57, 58, 58, 59, 59, 60,
    60, 61, 61, 62, 62, 63, 63, 64, 64, 65, 65, 66, 66, 67, 67, 68, 68, 69, 69, 70,
    70, 71, 71, 72, 72, 73, 73, 74, 74, 75, 75, 76, 76, 77, 77, 78, 78, 79, 79, 80,
    80, 81, 81, 82, 82, 83, 83, 84, 84, 85, 85, 86, 86, 87, 87, 88, 88, 89, 89, 90,
    90, 91, 91, 92, 92, 93, 93, 94, 94, 95, 95, 96, 96, 97, 97, 98, 98, 99, 99, 100,
    100, 101, 101, 102, 102, 103, 103, 104, 104, 105, 105, 106, 106, 107, 107, 108, 108, 109, 109, 110,
    110, 111, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127, 128, 128, 129, 129, 130,
    130, 131, 131, 132, 132, 133, 133, 134, 134, 135, 135, 136, 136, 137, 137, 138, 138, 139, 139, 140,
    140, 141, 141, 142, 142, 143, 143, 144, 144, 145, 145, 146, 146, 147, 147, 148, 148, 149, 149, 150,
    150, 151, 151, 152, 152, 153, 153, 154, 154, 155, 155, 156, 156, 157, 157, 158, 158, 159, 159, 160,
    160, 161, 161, 162, 162, 163, 163, 164, 164, 165, 165, 166, 166, 167, 167, 168, 168, 169, 169, 170,
    170, 171, 171, 172, 172, 173, 173, 174, 174, 175, 175, 176, 176, 177, 177, 178, 178, 179, 179, 180,
    180, 181, 181, 182, 182, 183, 183, 184, 184, 185, 185, 186, 186, 187, 187, 188, 188, 189, 189, 190,
    190, 191, 191, 192, 192, 193, 193, 194, 194, 195, 195, 196, 196, 197, 197, 198, 198, 199, 199, 200,
    200, 201, 201, 202, 202, 203, 203, 204, 204, 205, 205, 206, 206, 207, 207, 208, 208, 209, 209, 210,
    210, 211, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216, 216, 217, 217, 218, 218, 219, 219, 220,
    220, 221, 221, 222, 222, 223, 223, 224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230,
    230, 231, 231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239, 240,
    240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248, 248, 249, 249, 250,
    250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255
};


/* 6-tap filter of a..f (registers) into r12: clip((a - 5b + 20c + 20d - 5e + f + 16) >> 5),
 * computed as (a + f) + 5 * (4 * (c + d) - (b + e)); the rounding, shift and clipping are one
 * table lookup (lr = table, see h264arm_clip6tbl). Uses r11 and r12. */
#define F6(a, b, c, d, e, f) \
"   add     r11, " c ", " d "\n" \
"   add     r12, " b ", " e "\n" \
"   rsb     r11, r12, r11, lsl #2\n" \
"   add     r12, " a ", " f "\n" \
"   add     r12, r12, r11\n" \
"   add     r12, r12, r11, lsl #2\n" \
"   ldrb    r12, [lr, r12, asr #4]\n"
/* r12 = (r12 + g + 1) >> 1 */
#define AVGR(g) \
"   add     r12, r12, " g "\n" \
"   add     r12, r12, #1\n" \
"   mov     r12, r12, lsr #1\n"
/* r12 = (r12 + byte at addr + 1) >> 1 */
#define AVGM(addr) \
"   ldrb    r11, " addr "\n" \
"   add     r12, r12, r11\n" \
"   add     r12, r12, #1\n" \
"   mov     r12, r12, lsr #1\n"

__asm__(
"   .text\n"
"   .align 2\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_luma_lt4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm)
 * Normal luma filter (bS 1..3). prm = { alpha, beta, tc0 }.
 * r0 data, r1 across, r2 along, r3 count, r4 alpha, r5 beta, r6 tc0,
 * r7 p1, r8 p0, r9 q0, r10 q1, r11/r12 temps, lr tc (tc0 + number of filtered p1/q1)
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_luma_lt4\n"
"h264arm_luma_lt4:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   ldr     r4, [sp, #36]\n"
"   ldmia   r4, {r4, r5, r6}\n"
"1:\n"
"   ldrb    r8, [r0, -r1]\n"
"   ldrb    r9, [r0]\n"
"   ldrb    r7, [r0, -r1, lsl #1]\n"
"   ldrb    r10, [r0, r1]\n"
"   subs    r11, r8, r9\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r4\n"
"   bhs     5f\n"
"   subs    r11, r7, r8\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     5f\n"
"   subs    r11, r10, r9\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     5f\n"
"   mov     lr, r6\n"
    /* p side: |p2 - p0| < beta -> p1 += clip(-tc0, tc0, (p2 + ((p0+q0+1)>>1) - 2*p1) >> 1) */
"   sub     r11, r0, r1\n"
"   ldrb    r12, [r11, -r1, lsl #1]\n"
"   subs    r11, r12, r8\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     2f\n"
"   add     r11, r8, r9\n"
"   add     r11, r11, #1\n"
"   add     r12, r12, r11, lsr #1\n"
"   sub     r12, r12, r7, lsl #1\n"
"   mov     r12, r12, asr #1\n"
"   cmp     r12, r6\n"
"   movgt   r12, r6\n"
"   cmn     r12, r6\n"
"   rsblt   r12, r6, #0\n"
"   add     r12, r7, r12\n"
"   strb    r12, [r0, -r1, lsl #1]\n"
"   add     lr, lr, #1\n"
"2:\n"
    /* q side */
"   ldrb    r12, [r0, r1, lsl #1]\n"
"   subs    r11, r12, r9\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     3f\n"
"   add     r11, r8, r9\n"
"   add     r11, r11, #1\n"
"   add     r12, r12, r11, lsr #1\n"
"   sub     r12, r12, r10, lsl #1\n"
"   mov     r12, r12, asr #1\n"
"   cmp     r12, r6\n"
"   movgt   r12, r6\n"
"   cmn     r12, r6\n"
"   rsblt   r12, r6, #0\n"
"   add     r12, r10, r12\n"
"   strb    r12, [r0, r1]\n"
"   add     lr, lr, #1\n"
"3:\n"
    /* delta = clip(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3) */
"   sub     r11, r9, r8\n"
"   sub     r12, r7, r10\n"
"   add     r12, r12, r11, lsl #2\n"
"   add     r12, r12, #4\n"
"   mov     r12, r12, asr #3\n"
"   cmp     r12, lr\n"
"   movgt   r12, lr\n"
"   cmn     r12, lr\n"
"   rsblt   r12, lr, #0\n"
"   add     r8, r8, r12\n"
"   sub     r9, r9, r12\n"
"   cmp     r8, #255\n"
"   mvnhi   r8, r8, asr #31\n"
"   cmp     r9, #255\n"
"   mvnhi   r9, r9, asr #31\n"
"   strb    r8, [r0, -r1]\n"
"   strb    r9, [r0]\n"
"5:\n"
"   add     r0, r0, r2\n"
"   subs    r3, r3, #1\n"
"   bne     1b\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_luma_eq4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm)
 * Strong luma filter (bS 4). prm = { alpha, beta }.
 * r0 data, r1 across, r2 along, r3 temp, r4 alpha, r5 beta, r6 (alpha >> 2) + 2,
 * r7 p1, r8 p0, r9 q0, r10 q1, r11/r12 temps, lr strong flag; end pointer at [sp]
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_luma_eq4\n"
"h264arm_luma_eq4:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   ldr     r4, [sp, #36]\n"
"   ldmia   r4, {r4, r5}\n"
"   mov     r6, r4, lsr #2\n"
"   add     r6, r6, #2\n"
"   mul     r3, r2, r3\n"
"   add     r3, r0, r3\n"
"   stmfd   sp!, {r3}\n"
"1:\n"
"   ldrb    r8, [r0, -r1]\n"
"   ldrb    r9, [r0]\n"
"   ldrb    r7, [r0, -r1, lsl #1]\n"
"   ldrb    r10, [r0, r1]\n"
"   subs    r11, r7, r8\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     5f\n"
"   subs    r11, r10, r9\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     5f\n"
"   subs    r11, r8, r9\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r4\n"
"   bhs     5f\n"
"   cmp     r11, r6\n"
"   movlo   lr, #1\n"
"   movhs   lr, #0\n"
    /* p side */
"   bhs     2f\n"
"   sub     r11, r0, r1\n"
"   ldrb    r12, [r11, -r1, lsl #1]\n"      /* p2 */
"   subs    r11, r12, r8\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     2f\n"
"   add     r11, r7, r8\n"
"   add     r11, r11, r9\n"                 /* tmp = p1 + p0 + q0 */
"   add     r3, r12, r11, lsl #1\n"
"   add     r3, r3, r10\n"
"   add     r3, r3, #4\n"
"   mov     r3, r3, lsr #3\n"
"   strb    r3, [r0, -r1]\n"                /* p0 = (p2 + 2*tmp + q1 + 4) >> 3 */
"   add     r3, r12, r11\n"
"   add     r3, r3, #2\n"
"   mov     r3, r3, lsr #2\n"
"   strb    r3, [r0, -r1, lsl #1]\n"        /* p1 = (p2 + tmp + 2) >> 2 */
"   sub     r3, r0, r1, lsl #2\n"
"   ldrb    r3, [r3]\n"                     /* p3 */
"   add     r11, r11, #4\n"
"   add     r11, r11, r12\n"
"   add     r11, r11, r12, lsl #1\n"
"   add     r11, r11, r3, lsl #1\n"
"   mov     r11, r11, lsr #3\n"
"   sub     r3, r0, r1\n"
"   strb    r11, [r3, -r1, lsl #1]\n"       /* p2 = (2*p3 + 3*p2 + tmp + 4) >> 3 */
"   b       3f\n"
"2:\n"
"   add     r11, r8, r7, lsl #1\n"
"   add     r11, r11, r10\n"
"   add     r11, r11, #2\n"
"   mov     r11, r11, lsr #2\n"
"   strb    r11, [r0, -r1]\n"               /* p0 = (2*p1 + p0 + q1 + 2) >> 2 */
"3:\n"
    /* q side */
"   cmp     lr, #0\n"
"   beq     4f\n"
"   ldrb    r12, [r0, r1, lsl #1]\n"        /* q2 */
"   subs    r11, r12, r9\n"
"   rsbmi   r11, r11, #0\n"
"   cmp     r11, r5\n"
"   bhs     4f\n"
"   add     r11, r8, r9\n"
"   add     r11, r11, r10\n"                /* tmp = p0 + q0 + q1 */
"   add     r3, r12, r11, lsl #1\n"
"   add     r3, r3, r7\n"
"   add     r3, r3, #4\n"
"   mov     r3, r3, lsr #3\n"
"   strb    r3, [r0]\n"                     /* q0 = (p1 + 2*tmp + q2 + 4) >> 3 */
"   add     r3, r12, r11\n"
"   add     r3, r3, #2\n"
"   mov     r3, r3, lsr #2\n"
"   strb    r3, [r0, r1]\n"                 /* q1 = (tmp + q2 + 2) >> 2 */
"   add     r3, r0, r1\n"
"   ldrb    r3, [r3, r1, lsl #1]\n"         /* q3 */
"   add     r11, r11, #4\n"
"   add     r11, r11, r12\n"
"   add     r11, r11, r12, lsl #1\n"
"   add     r11, r11, r3, lsl #1\n"
"   mov     r11, r11, lsr #3\n"
"   strb    r11, [r0, r1, lsl #1]\n"        /* q2 = (2*q3 + 3*q2 + tmp + 4) >> 3 */
"   b       5f\n"
"4:\n"
"   add     r11, r9, r10, lsl #1\n"
"   add     r11, r11, r7\n"
"   add     r11, r11, #2\n"
"   mov     r11, r11, lsr #2\n"
"   strb    r11, [r0]\n"                    /* q0 = (2*q1 + q0 + p1 + 2) >> 2 */
"5:\n"
"   add     r0, r0, r2\n"
"   ldr     r3, [sp]\n"
"   cmp     r0, r3\n"
"   bne     1b\n"
"   add     sp, sp, #4\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_chroma_lt4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm)
 * Chroma filter, bS 1..3. prm = { alpha, beta, tc0 + 1 }.
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_chroma_lt4\n"
"h264arm_chroma_lt4:\n"
"   stmfd   sp!, {r4-r10}\n"
"   ldr     r4, [sp, #28]\n"
"   ldmia   r4, {r4, r5, r6}\n"
"1:\n"
"   ldrb    r8, [r0, -r1]\n"
"   ldrb    r9, [r0]\n"
"   ldrb    r7, [r0, -r1, lsl #1]\n"
"   ldrb    r10, [r0, r1]\n"
"   subs    r12, r8, r9\n"
"   rsbmi   r12, r12, #0\n"
"   cmp     r12, r4\n"
"   bhs     5f\n"
"   subs    r12, r7, r8\n"
"   rsbmi   r12, r12, #0\n"
"   cmp     r12, r5\n"
"   bhs     5f\n"
"   subs    r12, r10, r9\n"
"   rsbmi   r12, r12, #0\n"
"   cmp     r12, r5\n"
"   bhs     5f\n"
"   sub     r12, r9, r8\n"
"   sub     r7, r7, r10\n"
"   add     r12, r7, r12, lsl #2\n"
"   add     r12, r12, #4\n"
"   mov     r12, r12, asr #3\n"
"   cmp     r12, r6\n"
"   movgt   r12, r6\n"
"   cmn     r12, r6\n"
"   rsblt   r12, r6, #0\n"
"   add     r8, r8, r12\n"
"   sub     r9, r9, r12\n"
"   cmp     r8, #255\n"
"   mvnhi   r8, r8, asr #31\n"
"   cmp     r9, #255\n"
"   mvnhi   r9, r9, asr #31\n"
"   strb    r8, [r0, -r1]\n"
"   strb    r9, [r0]\n"
"5:\n"
"   add     r0, r0, r2\n"
"   subs    r3, r3, #1\n"
"   bne     1b\n"
"   ldmfd   sp!, {r4-r10}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_chroma_eq4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm)
 * Chroma filter, bS 4. prm = { alpha, beta }.
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_chroma_eq4\n"
"h264arm_chroma_eq4:\n"
"   stmfd   sp!, {r4-r10}\n"
"   ldr     r4, [sp, #28]\n"
"   ldmia   r4, {r4, r5}\n"
"1:\n"
"   ldrb    r8, [r0, -r1]\n"
"   ldrb    r9, [r0]\n"
"   ldrb    r7, [r0, -r1, lsl #1]\n"
"   ldrb    r10, [r0, r1]\n"
"   subs    r12, r8, r9\n"
"   rsbmi   r12, r12, #0\n"
"   cmp     r12, r4\n"
"   bhs     5f\n"
"   subs    r12, r7, r8\n"
"   rsbmi   r12, r12, #0\n"
"   cmp     r12, r5\n"
"   bhs     5f\n"
"   subs    r12, r10, r9\n"
"   rsbmi   r12, r12, #0\n"
"   cmp     r12, r5\n"
"   bhs     5f\n"
"   add     r12, r8, r7, lsl #1\n"
"   add     r12, r12, r10\n"
"   add     r12, r12, #2\n"
"   mov     r12, r12, lsr #2\n"
"   strb    r12, [r0, -r1]\n"               /* p0 = (2*p1 + p0 + q1 + 2) >> 2 */
"   add     r12, r9, r10, lsl #1\n"
"   add     r12, r12, r7\n"
"   add     r12, r12, #2\n"
"   mov     r12, r12, lsr #2\n"
"   strb    r12, [r0]\n"                    /* q0 = (2*q1 + q0 + p1 + 2) >> 2 */
"5:\n"
"   add     r0, r0, r2\n"
"   subs    r3, r3, #1\n"
"   bne     1b\n"
"   ldmfd   sp!, {r4-r10}\n"
"   bx      lr\n"
/* ------------------------------------------------------------------------------------------
 * void h264arm_copy_block(const u8 *src, u8 *dst, u32 srcStride, u32 w, u32 h, u32 dstStride)
 * Copies a w x h block (w = 4, 8 or 16; dst and dstStride multiples of 4; src any alignment).
 * Unaligned sources are read as aligned words and shifted together (every word read contains
 * at least one byte of the block, so nothing outside the picture's pages is touched).
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_copy_block\n"
"h264arm_copy_block:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   ldr     r4, [sp, #36]\n"
"   ldr     r5, [sp, #40]\n"
"   ands    r6, r0, #3\n"
"   bne     20f\n"
"   cmp     r3, #16\n"
"   bne     12f\n"
"11:\n"
"   ldmia   r0, {r8-r11}\n"
"   add     r0, r0, r2\n"
"   stmia   r1, {r8-r11}\n"
"   add     r1, r1, r5\n"
"   subs    r4, r4, #1\n"
"   bne     11b\n"
"   b       99f\n"
"12:\n"
"   cmp     r3, #8\n"
"   bne     14f\n"
"13:\n"
"   ldmia   r0, {r8-r9}\n"
"   add     r0, r0, r2\n"
"   stmia   r1, {r8-r9}\n"
"   add     r1, r1, r5\n"
"   subs    r4, r4, #1\n"
"   bne     13b\n"
"   b       99f\n"
"14:\n"
"   ldr     r8, [r0], r2\n"
"   str     r8, [r1], r5\n"
"   subs    r4, r4, #1\n"
"   bne     14b\n"
"   b       99f\n"
"20:\n"
"   bic     r0, r0, #3\n"
"   mov     r6, r6, lsl #3\n"
"   rsb     r7, r6, #32\n"
"   cmp     r3, #16\n"
"   bne     22f\n"
"21:\n"
"   ldmia   r0, {r8-r12}\n"
"   add     r0, r0, r2\n"
"   mov     r8, r8, lsr r6\n"
"   orr     r8, r8, r9, lsl r7\n"
"   mov     r9, r9, lsr r6\n"
"   orr     r9, r9, r10, lsl r7\n"
"   mov     r10, r10, lsr r6\n"
"   orr     r10, r10, r11, lsl r7\n"
"   mov     r11, r11, lsr r6\n"
"   orr     r11, r11, r12, lsl r7\n"
"   stmia   r1, {r8-r11}\n"
"   add     r1, r1, r5\n"
"   subs    r4, r4, #1\n"
"   bne     21b\n"
"   b       99f\n"
"22:\n"
"   cmp     r3, #8\n"
"   bne     24f\n"
"23:\n"
"   ldmia   r0, {r8-r10}\n"
"   add     r0, r0, r2\n"
"   mov     r8, r8, lsr r6\n"
"   orr     r8, r8, r9, lsl r7\n"
"   mov     r9, r9, lsr r6\n"
"   orr     r9, r9, r10, lsl r7\n"
"   stmia   r1, {r8-r9}\n"
"   add     r1, r1, r5\n"
"   subs    r4, r4, #1\n"
"   bne     23b\n"
"   b       99f\n"
"24:\n"
"   ldmia   r0, {r8-r9}\n"
"   add     r0, r0, r2\n"
"   mov     r8, r8, lsr r6\n"
"   orr     r8, r8, r9, lsl r7\n"
"   str     r8, [r1], r5\n"
"   subs    r4, r4, #1\n"
"   bne     24b\n"
"99:\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"
/* ------------------------------------------------------------------------------------------
 * void h264arm_hor6(const u8 *src, u8 *dst, i32 srcStride, u32 whm, const u8 *tbl)
 * Horizontal 6-tap luma interpolation of a w x h block (w = 4, 8, 16; whm = w | h << 8 |
 * mode << 16) into dst (line length 16). src points 2 pixels left of the block. tbl =
 * h264arm_clip6tbl + 160. mode 0: half-pel b; 1: average with the integer pixel on the left
 * (a); 2: with the one on the right (c); 3: average with what dst holds.
 * r0 src, r1 dst, r2-r10 nine source pixels, r11/r12 temps, lr table.
 * Stack: [0] w, [4] src step to the next line, [8] dst step, [12] lines left, [16] line end
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_hor6\n"
"h264arm_hor6:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   ldr     lr, [sp, #36]\n"
"   sub     sp, sp, #20\n"
"   and     r4, r3, #255\n"
"   str     r4, [sp]\n"
"   sub     r5, r2, r4\n"
"   str     r5, [sp, #4]\n"
"   rsb     r5, r4, #16\n"
"   str     r5, [sp, #8]\n"
"   mov     r5, r3, lsr #8\n"
"   and     r5, r5, #255\n"
"   str     r5, [sp, #12]\n"
"   add     r5, r1, r4\n"
"   str     r5, [sp, #16]\n"
"   mov     r3, r3, lsr #16\n"
"   cmp     r3, #1\n"
"   beq     h6m1\n"
"   cmp     r3, #2\n"
"   beq     h6m2\n"
"   cmp     r3, #3\n"
"   beq     h6m3\n"
"h6m0:\n"
"   ldrb    r2, [r0, #0]\n"
"   ldrb    r3, [r0, #1]\n"
"   ldrb    r4, [r0, #2]\n"
"   ldrb    r5, [r0, #3]\n"
"   ldrb    r6, [r0, #4]\n"
"   ldrb    r7, [r0, #5]\n"
"   ldrb    r8, [r0, #6]\n"
"   ldrb    r9, [r0, #7]\n"
"   ldrb    r10, [r0, #8]\n"
"   add     r0, r0, #4\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
"   strb    r12, [r1, #0]\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
"   strb    r12, [r1, #1]\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
"   strb    r12, [r1, #2]\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
"   strb    r12, [r1, #3]\n"
"   add     r1, r1, #4\n"
"   ldr     r11, [sp, #16]\n"
"   cmp     r1, r11\n"
"   bne     h6m0\n"
"   ldr     r11, [sp, #4]\n"
"   add     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   add     r1, r1, r11\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   beq     h6done\n"
"   ldr     r11, [sp]\n"
"   add     r11, r1, r11\n"
"   str     r11, [sp, #16]\n"
"   b       h6m0\n"
"h6m1:\n"
"   ldrb    r2, [r0, #0]\n"
"   ldrb    r3, [r0, #1]\n"
"   ldrb    r4, [r0, #2]\n"
"   ldrb    r5, [r0, #3]\n"
"   ldrb    r6, [r0, #4]\n"
"   ldrb    r7, [r0, #5]\n"
"   ldrb    r8, [r0, #6]\n"
"   ldrb    r9, [r0, #7]\n"
"   ldrb    r10, [r0, #8]\n"
"   add     r0, r0, #4\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
AVGR("r4")
"   strb    r12, [r1, #0]\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
AVGR("r5")
"   strb    r12, [r1, #1]\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
AVGR("r6")
"   strb    r12, [r1, #2]\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
AVGR("r7")
"   strb    r12, [r1, #3]\n"
"   add     r1, r1, #4\n"
"   ldr     r11, [sp, #16]\n"
"   cmp     r1, r11\n"
"   bne     h6m1\n"
"   ldr     r11, [sp, #4]\n"
"   add     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   add     r1, r1, r11\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   beq     h6done\n"
"   ldr     r11, [sp]\n"
"   add     r11, r1, r11\n"
"   str     r11, [sp, #16]\n"
"   b       h6m1\n"
"h6m2:\n"
"   ldrb    r2, [r0, #0]\n"
"   ldrb    r3, [r0, #1]\n"
"   ldrb    r4, [r0, #2]\n"
"   ldrb    r5, [r0, #3]\n"
"   ldrb    r6, [r0, #4]\n"
"   ldrb    r7, [r0, #5]\n"
"   ldrb    r8, [r0, #6]\n"
"   ldrb    r9, [r0, #7]\n"
"   ldrb    r10, [r0, #8]\n"
"   add     r0, r0, #4\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
AVGR("r5")
"   strb    r12, [r1, #0]\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
AVGR("r6")
"   strb    r12, [r1, #1]\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
AVGR("r7")
"   strb    r12, [r1, #2]\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
AVGR("r8")
"   strb    r12, [r1, #3]\n"
"   add     r1, r1, #4\n"
"   ldr     r11, [sp, #16]\n"
"   cmp     r1, r11\n"
"   bne     h6m2\n"
"   ldr     r11, [sp, #4]\n"
"   add     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   add     r1, r1, r11\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   beq     h6done\n"
"   ldr     r11, [sp]\n"
"   add     r11, r1, r11\n"
"   str     r11, [sp, #16]\n"
"   b       h6m2\n"
"h6m3:\n"
"   ldrb    r2, [r0, #0]\n"
"   ldrb    r3, [r0, #1]\n"
"   ldrb    r4, [r0, #2]\n"
"   ldrb    r5, [r0, #3]\n"
"   ldrb    r6, [r0, #4]\n"
"   ldrb    r7, [r0, #5]\n"
"   ldrb    r8, [r0, #6]\n"
"   ldrb    r9, [r0, #7]\n"
"   ldrb    r10, [r0, #8]\n"
"   add     r0, r0, #4\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
AVGM("[r1, #0]")
"   strb    r12, [r1, #0]\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
AVGM("[r1, #1]")
"   strb    r12, [r1, #1]\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
AVGM("[r1, #2]")
"   strb    r12, [r1, #2]\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
AVGM("[r1, #3]")
"   strb    r12, [r1, #3]\n"
"   add     r1, r1, #4\n"
"   ldr     r11, [sp, #16]\n"
"   cmp     r1, r11\n"
"   bne     h6m3\n"
"   ldr     r11, [sp, #4]\n"
"   add     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   add     r1, r1, r11\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   beq     h6done\n"
"   ldr     r11, [sp]\n"
"   add     r11, r1, r11\n"
"   str     r11, [sp, #16]\n"
"   b       h6m3\n"
"h6done:\n"
"   add     sp, sp, #20\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_ver6(const u8 *src, u8 *dst, i32 srcStride, u32 whm, const u8 *tbl)
 * Vertical 6-tap luma interpolation, like h264arm_hor6; src points 2 lines above the block.
 * mode 1: average with the integer pixel above (d), 2: below (n), 3: with dst.
 * Works column by column, 4 output lines per step.
 * Stack: [0] groups per column, [4] h*stride - 1, [8] h*16 - 1, [12] columns left,
 * [20] groups left, [24] stride, [28] 5*stride
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_ver6\n"
"h264arm_ver6:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   ldr     lr, [sp, #36]\n"
"   sub     sp, sp, #32\n"
"   str     r2, [sp, #24]\n"
"   add     r4, r2, r2, lsl #2\n"
"   str     r4, [sp, #28]\n"
"   and     r4, r3, #255\n"
"   str     r4, [sp, #12]\n"
"   mov     r5, r3, lsr #8\n"
"   and     r5, r5, #255\n"
"   mov     r6, r5, lsr #2\n"
"   str     r6, [sp]\n"
"   str     r6, [sp, #20]\n"
"   mul     r6, r5, r2\n"
"   sub     r6, r6, #1\n"
"   str     r6, [sp, #4]\n"
"   mov     r6, r5, lsl #4\n"
"   sub     r6, r6, #1\n"
"   str     r6, [sp, #8]\n"
"   mov     r3, r3, lsr #16\n"
"   cmp     r3, #1\n"
"   beq     v6m1\n"
"   cmp     r3, #2\n"
"   beq     v6m2\n"
"   cmp     r3, #3\n"
"   beq     v6m3\n"
"v6m0:\n"
"   ldr     r11, [sp, #24]\n"
"   ldrb    r2, [r0], r11\n"
"   ldrb    r3, [r0], r11\n"
"   ldrb    r4, [r0], r11\n"
"   ldrb    r5, [r0], r11\n"
"   ldrb    r6, [r0], r11\n"
"   ldrb    r7, [r0], r11\n"
"   ldrb    r8, [r0], r11\n"
"   ldrb    r9, [r0], r11\n"
"   ldrb    r10, [r0], r11\n"
"   ldr     r11, [sp, #28]\n"
"   sub     r0, r0, r11\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
"   strb    r12, [r1], #16\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
"   strb    r12, [r1], #16\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
"   strb    r12, [r1], #16\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
"   strb    r12, [r1], #16\n"
"   ldr     r11, [sp, #20]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #20]\n"
"   bne     v6m0\n"
"   ldr     r11, [sp, #4]\n"
"   sub     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   sub     r1, r1, r11\n"
"   ldr     r11, [sp]\n"
"   str     r11, [sp, #20]\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   bne     v6m0\n"
"   b       v6done\n"
"v6m1:\n"
"   ldr     r11, [sp, #24]\n"
"   ldrb    r2, [r0], r11\n"
"   ldrb    r3, [r0], r11\n"
"   ldrb    r4, [r0], r11\n"
"   ldrb    r5, [r0], r11\n"
"   ldrb    r6, [r0], r11\n"
"   ldrb    r7, [r0], r11\n"
"   ldrb    r8, [r0], r11\n"
"   ldrb    r9, [r0], r11\n"
"   ldrb    r10, [r0], r11\n"
"   ldr     r11, [sp, #28]\n"
"   sub     r0, r0, r11\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
AVGR("r4")
"   strb    r12, [r1], #16\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
AVGR("r5")
"   strb    r12, [r1], #16\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
AVGR("r6")
"   strb    r12, [r1], #16\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
AVGR("r7")
"   strb    r12, [r1], #16\n"
"   ldr     r11, [sp, #20]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #20]\n"
"   bne     v6m1\n"
"   ldr     r11, [sp, #4]\n"
"   sub     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   sub     r1, r1, r11\n"
"   ldr     r11, [sp]\n"
"   str     r11, [sp, #20]\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   bne     v6m1\n"
"   b       v6done\n"
"v6m2:\n"
"   ldr     r11, [sp, #24]\n"
"   ldrb    r2, [r0], r11\n"
"   ldrb    r3, [r0], r11\n"
"   ldrb    r4, [r0], r11\n"
"   ldrb    r5, [r0], r11\n"
"   ldrb    r6, [r0], r11\n"
"   ldrb    r7, [r0], r11\n"
"   ldrb    r8, [r0], r11\n"
"   ldrb    r9, [r0], r11\n"
"   ldrb    r10, [r0], r11\n"
"   ldr     r11, [sp, #28]\n"
"   sub     r0, r0, r11\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
AVGR("r5")
"   strb    r12, [r1], #16\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
AVGR("r6")
"   strb    r12, [r1], #16\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
AVGR("r7")
"   strb    r12, [r1], #16\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
AVGR("r8")
"   strb    r12, [r1], #16\n"
"   ldr     r11, [sp, #20]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #20]\n"
"   bne     v6m2\n"
"   ldr     r11, [sp, #4]\n"
"   sub     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   sub     r1, r1, r11\n"
"   ldr     r11, [sp]\n"
"   str     r11, [sp, #20]\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   bne     v6m2\n"
"   b       v6done\n"
"v6m3:\n"
"   ldr     r11, [sp, #24]\n"
"   ldrb    r2, [r0], r11\n"
"   ldrb    r3, [r0], r11\n"
"   ldrb    r4, [r0], r11\n"
"   ldrb    r5, [r0], r11\n"
"   ldrb    r6, [r0], r11\n"
"   ldrb    r7, [r0], r11\n"
"   ldrb    r8, [r0], r11\n"
"   ldrb    r9, [r0], r11\n"
"   ldrb    r10, [r0], r11\n"
"   ldr     r11, [sp, #28]\n"
"   sub     r0, r0, r11\n"
F6("r2", "r3", "r4", "r5", "r6", "r7")
AVGM("[r1]")
"   strb    r12, [r1], #16\n"
F6("r3", "r4", "r5", "r6", "r7", "r8")
AVGM("[r1]")
"   strb    r12, [r1], #16\n"
F6("r4", "r5", "r6", "r7", "r8", "r9")
AVGM("[r1]")
"   strb    r12, [r1], #16\n"
F6("r5", "r6", "r7", "r8", "r9", "r10")
AVGM("[r1]")
"   strb    r12, [r1], #16\n"
"   ldr     r11, [sp, #20]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #20]\n"
"   bne     v6m3\n"
"   ldr     r11, [sp, #4]\n"
"   sub     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   sub     r1, r1, r11\n"
"   ldr     r11, [sp]\n"
"   str     r11, [sp, #20]\n"
"   ldr     r11, [sp, #12]\n"
"   subs    r11, r11, #1\n"
"   str     r11, [sp, #12]\n"
"   bne     v6m3\n"
"   b       v6done\n"
"v6done:\n"
"   add     sp, sp, #32\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"
/* ------------------------------------------------------------------------------------------
 * Chroma interpolation (bilinear, 1/8 pel) into the 8-wide prediction buffer.
 * void h264arm_chroma_hor(const u8 *src, u8 *dst, i32 stride, u32 whf)   whf = w | h << 8 | xFrac << 16
 * void h264arm_chroma_ver(const u8 *src, u8 *dst, i32 stride, u32 whf)   ... | yFrac << 16
 * Both: out = ((8 - f) * a + f * b + 4) >> 3, the same as h264bsd's (((..) << 3) + 32) >> 6.
 * One loop for both: r2/r3 inner source/destination step, r4 8 - f, r5 f, r6 inner count,
 * r7 lines, r8/r9 source/destination step from the end of a line to the start of the next.
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_chroma_hor\n"
"h264arm_chroma_hor:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   and     r6, r3, #255\n"            /* n = w */
"   mov     r7, r3, lsr #8\n"
"   and     r7, r7, #255\n"            /* m = h */
"   mov     r5, r3, lsr #16\n"
"   and     r5, r5, #7\n"
"   rsb     r4, r5, #8\n"
"   sub     r8, r2, r6\n"
"   sub     r8, r8, #1\n"              /* stride - (w + 1) */
"   rsb     r9, r6, #8\n"              /* 8 - w */
"   mov     r2, #1\n"
"   mov     r3, #1\n"
"   b       c1outer\n"
"   .global h264arm_chroma_ver\n"
"h264arm_chroma_ver:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   and     r7, r3, #255\n"            /* m = w */
"   mov     r6, r3, lsr #8\n"
"   and     r6, r6, #255\n"            /* n = h */
"   mov     r5, r3, lsr #16\n"
"   and     r5, r5, #7\n"
"   rsb     r4, r5, #8\n"
"   add     r8, r6, #1\n"
"   mul     r9, r8, r2\n"
"   rsb     r8, r9, #1\n"              /* 1 - (h + 1) * stride */
"   mov     r9, r6, lsl #3\n"
"   rsb     r9, r9, #1\n"              /* 1 - 8h */
"   mov     r3, #8\n"
"c1outer:\n"
"   ldrb    r10, [r0], r2\n"
"   mov     lr, r6\n"
"c1inner:\n"
"   ldrb    r11, [r0], r2\n"
"   mul     r12, r10, r4\n"
"   mla     r12, r11, r5, r12\n"
"   add     r12, r12, #4\n"
"   mov     r12, r12, lsr #3\n"
"   strb    r12, [r1], r3\n"
"   ldrb    r10, [r0], r2\n"
"   mul     r12, r11, r4\n"
"   mla     r12, r10, r5, r12\n"
"   add     r12, r12, #4\n"
"   mov     r12, r12, lsr #3\n"
"   strb    r12, [r1], r3\n"
"   subs    lr, lr, #2\n"
"   bne     c1inner\n"
"   add     r0, r0, r8\n"
"   add     r1, r1, r9\n"
"   subs    r7, r7, #1\n"
"   bne     c1outer\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_chroma_hv(const u8 *src, u8 *dst, i32 stride, u32 whxy)
 * whxy = w | h << 8 | xFrac << 16 | yFrac << 24; out = (valX * v(x) + xFrac * v(x + 1) + 32)
 * >> 6 with v = valY * top + yFrac * bottom, as in h264bsd.
 * r3 valX, r4 xFrac, r5 valY, r6 yFrac, r7/r8 v of the left/right column, r12 lines left.
 * Stack: [0] w, [4] stride - w, [8] 8 - w
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_chroma_hv\n"
"h264arm_chroma_hv:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   sub     sp, sp, #12\n"
"   and     r7, r3, #255\n"
"   str     r7, [sp]\n"
"   sub     r8, r2, r7\n"
"   str     r8, [sp, #4]\n"
"   rsb     r8, r7, #8\n"
"   str     r8, [sp, #8]\n"
"   mov     r12, r3, lsr #8\n"
"   and     r12, r12, #255\n"
"   mov     r4, r3, lsr #16\n"
"   and     r4, r4, #7\n"
"   mov     r6, r3, lsr #24\n"
"   and     r6, r6, #7\n"
"   rsb     r3, r4, #8\n"
"   rsb     r5, r6, #8\n"
"hvrow:\n"
"   ldrb    r9, [r0]\n"
"   ldrb    r10, [r0, r2]\n"
"   mul     r7, r9, r5\n"
"   mla     r7, r10, r6, r7\n"
"   ldr     lr, [sp]\n"
"hvcol:\n"
"   ldrb    r9, [r0, #1]!\n"
"   ldrb    r10, [r0, r2]\n"
"   mul     r8, r9, r5\n"
"   mla     r8, r10, r6, r8\n"
"   mul     r11, r7, r3\n"
"   mla     r11, r8, r4, r11\n"
"   add     r11, r11, #32\n"
"   mov     r11, r11, lsr #6\n"
"   strb    r11, [r1], #1\n"
"   ldrb    r9, [r0, #1]!\n"
"   ldrb    r10, [r0, r2]\n"
"   mul     r7, r9, r5\n"
"   mla     r7, r10, r6, r7\n"
"   mul     r11, r8, r3\n"
"   mla     r11, r7, r4, r11\n"
"   add     r11, r11, #32\n"
"   mov     r11, r11, lsr #6\n"
"   strb    r11, [r1], #1\n"
"   subs    lr, lr, #2\n"
"   bne     hvcol\n"
"   ldr     r11, [sp, #4]\n"
"   add     r0, r0, r11\n"
"   ldr     r11, [sp, #8]\n"
"   add     r1, r1, r11\n"
"   subs    r12, r12, #1\n"
"   bne     hvrow\n"
"   add     sp, sp, #12\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"
/* ------------------------------------------------------------------------------------------
 * u32 h264arm_idct4x4(i32 *data, const i32 *scale)
 * h264bsdProcessBlock's general case: inverse zig-zag + dequantisation (scale = { t1, t2, t3 },
 * data[0] already scaled) and the 4x4 inverse transform, result in data. Returns 1 (NOK) if a
 * result is outside [-512, 511] (the columns before it are written, like the C code).
 * Rows go to a 64-byte stack buffer, the columns back to data.
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_idct4x4\n"
"h264arm_idct4x4:\n"
"   stmfd   sp!, {r4-r11, lr}\n"
"   sub     sp, sp, #64\n"
"   ldmia   r1, {r1, r2, r3}\n"
"   mov     r12, sp\n"
"   ldr     r4, [r0, #0]\n"
"   ldr     r5, [r0, #4]\n"
"   ldr     r6, [r0, #20]\n"
"   ldr     r7, [r0, #24]\n"
"   mul     r5, r2, r5\n"
"   mul     r6, r1, r6\n"
"   mul     r7, r2, r7\n"
"   add     r8, r4, r6\n"
"   sub     r9, r4, r6\n"
"   mov     r10, r5, asr #1\n"
"   sub     r10, r10, r7\n"
"   add     r11, r5, r7, asr #1\n"
"   add     r4, r8, r11\n"
"   add     r5, r9, r10\n"
"   sub     r6, r9, r10\n"
"   sub     r7, r8, r11\n"
"   stmia   r12!, {r4-r7}\n"
"   ldr     r4, [r0, #8]\n"
"   ldr     r5, [r0, #16]\n"
"   ldr     r6, [r0, #28]\n"
"   ldr     r7, [r0, #48]\n"
"   mul     r4, r2, r4\n"
"   mul     r5, r3, r5\n"
"   mul     r6, r2, r6\n"
"   mul     r7, r3, r7\n"
"   add     r8, r4, r6\n"
"   sub     r9, r4, r6\n"
"   mov     r10, r5, asr #1\n"
"   sub     r10, r10, r7\n"
"   add     r11, r5, r7, asr #1\n"
"   add     r4, r8, r11\n"
"   add     r5, r9, r10\n"
"   sub     r6, r9, r10\n"
"   sub     r7, r8, r11\n"
"   stmia   r12!, {r4-r7}\n"
"   ldr     r4, [r0, #12]\n"
"   ldr     r5, [r0, #32]\n"
"   ldr     r6, [r0, #44]\n"
"   ldr     r7, [r0, #52]\n"
"   mul     r4, r1, r4\n"
"   mul     r5, r2, r5\n"
"   mul     r6, r1, r6\n"
"   mul     r7, r2, r7\n"
"   add     r8, r4, r6\n"
"   sub     r9, r4, r6\n"
"   mov     r10, r5, asr #1\n"
"   sub     r10, r10, r7\n"
"   add     r11, r5, r7, asr #1\n"
"   add     r4, r8, r11\n"
"   add     r5, r9, r10\n"
"   sub     r6, r9, r10\n"
"   sub     r7, r8, r11\n"
"   stmia   r12!, {r4-r7}\n"
"   ldr     r4, [r0, #36]\n"
"   ldr     r5, [r0, #40]\n"
"   ldr     r6, [r0, #56]\n"
"   ldr     r7, [r0, #60]\n"
"   mul     r4, r2, r4\n"
"   mul     r5, r3, r5\n"
"   mul     r6, r2, r6\n"
"   mul     r7, r3, r7\n"
"   add     r8, r4, r6\n"
"   sub     r9, r4, r6\n"
"   mov     r10, r5, asr #1\n"
"   sub     r10, r10, r7\n"
"   add     r11, r5, r7, asr #1\n"
"   add     r4, r8, r11\n"
"   add     r5, r9, r10\n"
"   sub     r6, r9, r10\n"
"   sub     r7, r8, r11\n"
"   stmia   r12!, {r4-r7}\n"
"   mov     r12, sp\n"
"   mov     r1, #0x8000\n"
"   add     r1, r1, #32\n"             /* 32 (rounding) + 512 * 64 (range check offset) */
"   mov     lr, #4\n"
"idcol:\n"
"   ldr     r4, [r12, #0]\n"
"   ldr     r5, [r12, #16]\n"
"   ldr     r6, [r12, #32]\n"
"   ldr     r7, [r12, #48]\n"
"   add     r8, r4, r6\n"
"   sub     r9, r4, r6\n"
"   mov     r10, r5, asr #1\n"
"   sub     r10, r10, r7\n"
"   add     r11, r5, r7, asr #1\n"
"   add     r8, r8, r1\n"
"   add     r9, r9, r1\n"
"   add     r4, r8, r11\n"
"   sub     r7, r8, r11\n"
"   add     r5, r9, r10\n"
"   sub     r6, r9, r10\n"
"   mov     r4, r4, asr #6\n"             /* value + 512 */
"   mov     r5, r5, asr #6\n"
"   mov     r6, r6, asr #6\n"
"   mov     r7, r7, asr #6\n"
"   orr     r3, r4, r5\n"
"   orr     r3, r3, r6\n"
"   orr     r3, r3, r7\n"
"   sub     r4, r4, #512\n"
"   sub     r5, r5, #512\n"
"   sub     r6, r6, #512\n"
"   sub     r7, r7, #512\n"
"   str     r4, [r0, #0]\n"
"   str     r5, [r0, #16]\n"
"   str     r6, [r0, #32]\n"
"   str     r7, [r0, #48]\n"
"   movs    r3, r3, lsr #10\n"            /* some value + 512 outside 0..1023 */
"   bne     idfail\n"
"   add     r0, r0, #4\n"
"   add     r12, r12, #4\n"
"   subs    lr, lr, #1\n"
"   bne     idcol\n"
"   mov     r0, #0\n"
"   add     sp, sp, #64\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"
"idfail:\n"
"   mov     r0, #1\n"
"   add     sp, sp, #64\n"
"   ldmfd   sp!, {r4-r11, lr}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_write_mb(u8 *lum, u8 *cb, u8 *cr, const u8 *data, u32 width)
 * Copies the predicted macroblock (data: 16x16 luma, then 8x8 cb and cr) into the picture;
 * width = luma line length. All pointers word aligned.
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_write_mb\n"
"h264arm_write_mb:\n"
"   stmfd   sp!, {r4-r9, lr}\n"
"   ldr     lr, [sp, #28]\n"
"   mov     r12, #16\n"
"wmb1:\n"
"   ldmia   r3!, {r4-r7}\n"
"   stmia   r0, {r4-r7}\n"
"   add     r0, r0, lr\n"
"   subs    r12, r12, #1\n"
"   bne     wmb1\n"
"   mov     lr, lr, lsr #1\n"
"   mov     r12, #4\n"
"wmb2:\n"
"   ldmia   r3!, {r4-r7}\n"
"   stmia   r1, {r4-r5}\n"
"   add     r1, r1, lr\n"
"   stmia   r1, {r6-r7}\n"
"   add     r1, r1, lr\n"
"   subs    r12, r12, #1\n"
"   bne     wmb2\n"
"   mov     r12, #4\n"
"wmb3:\n"
"   ldmia   r3!, {r4-r7}\n"
"   stmia   r2, {r4-r5}\n"
"   add     r2, r2, lr\n"
"   stmia   r2, {r6-r7}\n"
"   add     r2, r2, lr\n"
"   subs    r12, r12, #1\n"
"   bne     wmb3\n"
"   ldmfd   sp!, {r4-r9, lr}\n"
"   bx      lr\n"

/* ------------------------------------------------------------------------------------------
 * void h264arm_add4x4(u8 *dst, const i32 *res, u32 stride, const u8 *clip)
 * dst = clip(dst + res) for a 4x4 block (dst word aligned); clip = h264bsdClip + 512.
 * ------------------------------------------------------------------------------------------ */
"   .global h264arm_add4x4\n"
"h264arm_add4x4:\n"
"   stmfd   sp!, {r4-r9, lr}\n"
"   mov     lr, #4\n"
"add4:\n"
"   ldr     r9, [r0]\n"
"   ldmia   r1!, {r4-r7}\n"
"   and     r8, r9, #255\n"
"   add     r8, r8, r4\n"
"   ldrb    r4, [r3, r8]\n"
"   mov     r8, r9, lsr #8\n"
"   and     r8, r8, #255\n"
"   add     r8, r8, r5\n"
"   ldrb    r5, [r3, r8]\n"
"   orr     r4, r4, r5, lsl #8\n"
"   mov     r8, r9, lsr #16\n"
"   and     r8, r8, #255\n"
"   add     r8, r8, r6\n"
"   ldrb    r6, [r3, r8]\n"
"   orr     r4, r4, r6, lsl #16\n"
"   add     r8, r7, r9, lsr #24\n"
"   ldrb    r7, [r3, r8]\n"
"   orr     r4, r4, r7, lsl #24\n"
"   str     r4, [r0], r2\n"
"   subs    lr, lr, #1\n"
"   bne     add4\n"
"   ldmfd   sp!, {r4-r9, lr}\n"
"   bx      lr\n"
);



#else
/* nothing: the C code is used */
typedef int h264bsd_arm_unused;
#endif
