// Host-side tests: SPSC FIFO, clock divider math, and a cycle model of the
// I2S PIO program decoded by a reference I2S receiver.
// Build/run: ./tests/run_host_tests.sh

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pcm5101a_clock.h"
#include "pcm5101a_fifo.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_fifo(void) {
    int32_t store[2 * 8];
    pcm5101a_fifo_t f;
    CHECK(!pcm5101a_fifo_init(&f, store, 6));
    CHECK(pcm5101a_fifo_init(&f, store, 8));

    int32_t in[2 * 10], out[2 * 10];
    for (int i = 0; i < 20; i++) in[i] = i + 1;

    CHECK(pcm5101a_fifo_push(&f, in, 10) == 8);
    CHECK(pcm5101a_fifo_level(&f) == 8);
    CHECK(!pcm5101a_fifo_pop_exact(&f, out, 9));
    CHECK(pcm5101a_fifo_pop_exact(&f, out, 5));
    CHECK(memcmp(out, in, sizeof(int32_t) * 10) == 0);
    CHECK(pcm5101a_fifo_level(&f) == 3);

    // Wrap-around, including free-running index overflow.
    f.head = f.tail = 0xFFFFFFFEu;
    for (int round = 0; round < 50; round++) {
        CHECK(pcm5101a_fifo_push(&f, in, 3) == 3);
        CHECK(pcm5101a_fifo_pop_exact(&f, out, 3));
        CHECK(memcmp(out, in, sizeof(int32_t) * 6) == 0);
    }
    CHECK(pcm5101a_fifo_level(&f) == 0);
}

static void test_clkdiv(void) {
    pcm5101a_clkdiv_t d;
    CHECK(pcm5101a_compute_clkdiv(192000000u, 48000, &d));
    CHECK(d.div_int == 31 && d.div_frac == 64 && d.exact && d.error_ppb == 0);
    CHECK(pcm5101a_compute_clkdiv(192000000u, 16000, &d));
    CHECK(d.div_int == 93 && d.div_frac == 192 && d.exact);
    CHECK(pcm5101a_compute_clkdiv(192000000u, 24000, &d));
    CHECK(d.div_int == 62 && d.div_frac == 128 && d.exact);
    CHECK(pcm5101a_compute_clkdiv(192000000u, 32000, &d));
    CHECK(d.div_int == 46 && d.div_frac == 224 && d.exact);
    CHECK(pcm5101a_compute_clkdiv(192000000u, 44100, &d));
    CHECK(!d.exact && d.error_ppb > -100000 && d.error_ppb < 100000);  // < 100 ppm
    CHECK(!pcm5101a_compute_clkdiv(192000000u, 8000, &d));
    CHECK(!pcm5101a_compute_clkdiv(192000000u, 47999, &d));
}

// Cycle model of src/pcm5101a_i2s.pio. Must be kept in sync with the program.
// sideset bit0 = BCK, bit1 = LRCK. Starts at entry_point.
enum { OP_OUT, OP_JMP_X, OP_SET_X };
typedef struct { int op; int arg; int side; } instr_t;
static const instr_t prog[] = {
    {OP_OUT, 0, 0b00},   // 0 left
    {OP_JMP_X, 0, 0b01}, // 1
    {OP_OUT, 0, 0b10},   // 2
    {OP_SET_X, 30, 0b11},// 3
    {OP_OUT, 0, 0b10},   // 4 right
    {OP_JMP_X, 4, 0b11}, // 5
    {OP_OUT, 0, 0b00},   // 6
    {OP_SET_X, 30, 0b01},// 7 entry_point
};
#define PROG_LEN 8
#define ENTRY 7

static void test_pio_i2s_framing(void) {
    enum { FRAMES = 6 };
    uint32_t words[2 * FRAMES];
    for (int i = 0; i < 2 * FRAMES; i++) words[i] = 0x80000001u ^ (uint32_t)(i * 0x1234567u);

    int pc = ENTRY, x = 0, osr_count = 0, word_idx = 0, din = 0;
    uint32_t osr = 0;
    // prev_lrck = 1 models the idle "right" slot before the first frame.
    int prev_bck = 0, prev_lrck = 1, nbits = -1, cur_lr = 0;
    uint32_t shift = 0;
    uint32_t decoded[2 * FRAMES + 2];
    int ndecoded = 0, cycles = 0, bck_rises = 0;

    while (word_idx <= 2 * FRAMES && cycles < 100000) {
        const instr_t in = prog[pc];
        int next = (pc + 1) % PROG_LEN;
        if (in.op == OP_OUT) {
            if (osr_count == 0) {
                if (word_idx == 2 * FRAMES) break;
                osr = words[word_idx++];
                osr_count = 32;
            }
            din = (int)(osr >> 31);
            osr <<= 1;
            osr_count--;
        } else if (in.op == OP_JMP_X) {
            if (x != 0) next = in.arg;
            x--;
        } else {
            x = in.arg;
        }
        int bck = in.side & 1, lrck = (in.side >> 1) & 1;

        // Reference I2S receiver: latch DIN on BCK rising. The rising edge
        // that sees a new LRCK level latches the LSB of the previous word;
        // the MSB of the next word follows on the next rising edge.
        if (bck && !prev_bck) {
            bck_rises++;
            if (nbits >= 0) {
                shift = (shift << 1) | (uint32_t)din;
                nbits++;
            }
            if (lrck != prev_lrck) {
                if (nbits == 32) {
                    CHECK((ndecoded & 1) == cur_lr);  // left (LRCK=0) first
                    decoded[ndecoded++] = shift;
                } else {
                    CHECK(nbits == -1);  // only the start may be partial
                }
                nbits = 0;
                shift = 0;
                cur_lr = lrck;
            }
            prev_lrck = lrck;
        }
        prev_bck = bck;
        pc = next;
        cycles++;
    }

    CHECK(bck_rises > 0 && (cycles - 2 * bck_rises) <= 2 && (2 * bck_rises - cycles) <= 2);
    CHECK(ndecoded == 2 * FRAMES);
    for (int i = 0; i < ndecoded; i++) {
        CHECK(decoded[i] == words[i]);
    }
}

int main(void) {
    test_fifo();
    test_clkdiv();
    test_pio_i2s_framing();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
