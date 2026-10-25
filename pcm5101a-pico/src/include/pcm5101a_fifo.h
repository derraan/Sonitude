#pragma once

// Lock-free single-producer / single-consumer FIFO of stereo frames.
// Producer: pcm5101a_write() (any core / thread context).
// Consumer: the DMA IRQ handler.
// Pure C, no SDK dependencies, so it can be unit-tested on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t *buf;          // 2 * capacity int32 (interleaved L, R)
    uint32_t capacity;     // frames, power of two
    uint32_t head;         // written only by producer (free-running)
    uint32_t tail;         // written only by consumer (free-running)
} pcm5101a_fifo_t;

static inline bool pcm5101a_fifo_init(pcm5101a_fifo_t *f, int32_t *storage, uint32_t capacity_frames) {
    if (capacity_frames == 0 || (capacity_frames & (capacity_frames - 1)) != 0) {
        return false;
    }
    f->buf = storage;
    f->capacity = capacity_frames;
    f->head = 0;
    f->tail = 0;
    return true;
}

static inline uint32_t pcm5101a_fifo_level(const pcm5101a_fifo_t *f) {
    uint32_t head = __atomic_load_n(&f->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&f->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

// Copies up to `frames` frames in; returns frames accepted (never blocks).
static inline size_t pcm5101a_fifo_push(pcm5101a_fifo_t *f, const int32_t *lr, size_t frames) {
    uint32_t head = f->head;
    uint32_t tail = __atomic_load_n(&f->tail, __ATOMIC_ACQUIRE);
    uint32_t space = f->capacity - (head - tail);
    if (frames > space) {
        frames = space;
    }
    uint32_t mask = f->capacity - 1;
    for (size_t i = 0; i < frames; i++) {
        uint32_t idx = ((head + (uint32_t)i) & mask) * 2;
        f->buf[idx] = lr[2 * i];
        f->buf[idx + 1] = lr[2 * i + 1];
    }
    __atomic_store_n(&f->head, head + (uint32_t)frames, __ATOMIC_RELEASE);
    return frames;
}

// All-or-nothing pop of exactly `frames` frames; returns false if not enough.
static inline bool pcm5101a_fifo_pop_exact(pcm5101a_fifo_t *f, int32_t *lr, uint32_t frames) {
    uint32_t tail = f->tail;
    uint32_t head = __atomic_load_n(&f->head, __ATOMIC_ACQUIRE);
    if (head - tail < frames) {
        return false;
    }
    uint32_t mask = f->capacity - 1;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = ((tail + i) & mask) * 2;
        lr[2 * i] = f->buf[idx];
        lr[2 * i + 1] = f->buf[idx + 1];
    }
    __atomic_store_n(&f->tail, tail + frames, __ATOMIC_RELEASE);
    return true;
}
