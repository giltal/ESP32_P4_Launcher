/*
 * Benchmark kernels, included twice by main.c:
 *   SUF  = name suffix, CODE = placement of functions, DATA = placement of tables.
 */
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define K(name) CAT(name, SUF)

/* â”€â”€ 1. Bytecode interpreter: an emulator-style fetch/decode/execute loop â”€â”€ */

#ifndef KERNELS_OPS
#define KERNELS_OPS
enum { OP_LDI, OP_ADD, OP_SUB, OP_XOR, OP_SHL, OP_SHR, OP_LD, OP_ST, OP_DEC, OP_JNZ, OP_MOV, OP_AND,
       OP_OR, OP_INC, OP_SWP, OP_HALT };
#endif

DATA static uint8_t K(vm_mem)[256];

/* Program: for (i = r0; i; i--) { r1 = ((r1 ^ mem[r1&255]) << 1 | r1 >> 7) + i; mem[i&255] = r1; } */
DATA static uint8_t K(vm_prog)[] = {
    OP_LDI, 0, 0,          /* r0 = iterations (patched)   */
    OP_LDI, 1, 0x5A,       /* r1 = seed                   */
    /* loop @6 */
    OP_MOV, 2, 1,          /* r2 = r1                     */
    OP_AND, 2, 255,        /* r2 &= 255                   */
    OP_LD, 3, 2,           /* r3 = mem[r2]                */
    OP_XOR, 1, 3,          /* r1 ^= r3                    */
    OP_MOV, 3, 1,
    OP_SHL, 1, 1,          /* r1 <<= 1                    */
    OP_SHR, 3, 7,          /* r3 >>= 7                    */
    OP_OR, 1, 3,           /* r1 |= r3                    */
    OP_ADD, 1, 0,          /* r1 += r0                    */
    OP_MOV, 2, 0,
    OP_AND, 2, 255,
    OP_ST, 2, 1,           /* mem[r2] = r1                */
    OP_DEC, 0, 0,
    OP_JNZ, 0, 6,          /* if (r0) goto loop           */
    OP_HALT, 0, 0,
};

CODE uint32_t K(vm_run)(uint32_t iters)
{
    uint32_t r[4] = { 0 };
    const uint8_t *prog = K(vm_prog);
    uint8_t *mem = K(vm_mem);
    uint32_t pc = 0;
    r[0] = iters;
    for (;;) {
        uint8_t op = prog[pc], a = prog[pc + 1], b = prog[pc + 2];
        pc += 3;
        switch (op) {
        case OP_LDI: if (pc == 3) r[a] = iters; else r[a] = b; break;
        case OP_ADD: r[a] += r[b]; break;
        case OP_SUB: r[a] -= r[b]; break;
        case OP_XOR: r[a] ^= r[b]; break;
        case OP_SHL: r[a] <<= b; break;
        case OP_SHR: r[a] >>= b; break;
        case OP_LD:  r[a] = mem[r[b] & 255]; break;
        case OP_ST:  mem[r[a] & 255] = (uint8_t)r[b]; break;
        case OP_DEC: r[a]--; break;
        case OP_JNZ: if (r[a]) pc = b; break;
        case OP_MOV: r[a] = r[b]; break;
        case OP_AND: r[a] &= b; break;
        case OP_OR:  r[a] |= r[b]; break;
        case OP_INC: r[a]++; break;
        case OP_SWP: { uint32_t t = r[a]; r[a] = r[b]; r[b] = t; } break;
        default:     return r[1];
        }
    }
}

/* â”€â”€ 2. Table-driven CRC32 (table placement matters) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

DATA static uint32_t K(crc_tab)[256];

CODE void K(crc_init)(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        K(crc_tab)[i] = c;
    }
}

CODE uint32_t K(crc_run)(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = K(crc_tab)[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* â”€â”€ 3. 8-bit â†’ RGB565 palette blit (memory bound; buffers in PSRAM) â”€â”€â”€â”€â”€â”€â”€ */

DATA static uint16_t K(pal)[256];

CODE void K(blit)(uint16_t *dst, const uint8_t *src, size_t n)
{
    const uint16_t *pal = K(pal);
    for (size_t i = 0; i < n; i += 4) {
        dst[i] = pal[src[i]];
        dst[i + 1] = pal[src[i + 1]];
        dst[i + 2] = pal[src[i + 2]];
        dst[i + 3] = pal[src[i + 3]];
    }
}

/* â”€â”€ 4. Large code footprint: 32 distinct ~1 KB functions (> 16 KB L1 I$) â”€â”€ */

#define MIX(k)  x = (x ^ (x >> 7)) * (0x9E3779B1u + (k)); x += (x << 3) ^ (k);
#define MIX8(k) MIX(k) MIX(k + 1) MIX(k + 2) MIX(k + 3) MIX(k + 4) MIX(k + 5) MIX(k + 6) MIX(k + 7)
#define BIG(n)                                                                              \
    CODE __attribute__((noinline)) uint32_t K(big##n)(uint32_t x)                          \
    {                                                                                       \
        MIX8(n * 64) MIX8(n * 64 + 8) MIX8(n * 64 + 16) MIX8(n * 64 + 24)                   \
        MIX8(n * 64 + 32) MIX8(n * 64 + 40) MIX8(n * 64 + 48) MIX8(n * 64 + 56)             \
        return x;                                                                           \
    }
BIG(0) BIG(1) BIG(2) BIG(3) BIG(4) BIG(5) BIG(6) BIG(7) BIG(8) BIG(9) BIG(10) BIG(11) BIG(12)
BIG(13) BIG(14) BIG(15) BIG(16) BIG(17) BIG(18) BIG(19) BIG(20) BIG(21) BIG(22) BIG(23) BIG(24)
BIG(25) BIG(26) BIG(27) BIG(28) BIG(29) BIG(30) BIG(31)

static uint32_t (*const K(bigs)[32])(uint32_t) = {
    K(big0), K(big1), K(big2), K(big3), K(big4), K(big5), K(big6), K(big7), K(big8), K(big9),
    K(big10), K(big11), K(big12), K(big13), K(big14), K(big15), K(big16), K(big17), K(big18),
    K(big19), K(big20), K(big21), K(big22), K(big23), K(big24), K(big25), K(big26), K(big27),
    K(big28), K(big29), K(big30), K(big31),
};

CODE uint32_t K(big_run)(uint32_t rounds)
{
    uint32_t x = 1;
    for (uint32_t r = 0; r < rounds; r++)
        for (int i = 0; i < 32; i++) x = K(bigs)[i](x);
    return x;
}

/* ── 5. An emulator-like frame: CPU core + a lot of other memory traffic ────
 * Interpreter steps, the big code chain, and `chunk` bytes of fresh PSRAM
 * data per frame (ROM/VRAM streaming) that evict code lines from L2. */
CODE uint32_t K(frames)(uint32_t n, const uint8_t *data, size_t data_len, size_t chunk)
{
    uint32_t x = 0, off = 0;
    for (uint32_t f = 0; f < n; f++) {
        x += K(vm_run)(150);
        x = K(big_run)(1) ^ x;
        for (size_t i = 0; i < chunk; i += 64) x += data[(off + i) % data_len];
        off += chunk;
    }
    return x;
}

#undef BIG
#undef MIX8
#undef MIX
#undef K
