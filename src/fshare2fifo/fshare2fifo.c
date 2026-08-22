/*
 * fshare2fifo - forward the H.264 stream from the YI FH8626V100 shared
 * frame buffer (/dev/shm/fshare_frame_buf) to a fifo or file.
 *
 * The stock app (rmm) continuously writes the encoded H.264 stream into
 * the shared buffer and bumps a frame counter at offset 0x18. This tool
 * watches the counter and forwards new frames as they are written.
 *
 * Buffer layout (reverse-engineered on the FH8626V100, fw 5.0.00.00):
 *   0x00-0x3F   small header; u32 frame counter at 0x18
 *   ~0x46xx..   H.264 stream ring, wraps at the end of the buffer.
 *               Frames are chained by 3-byte start codes (00 00 01).
 *
 * Based on the design of h264grabber.c (GPL-3) by roleoroleo / yi-hack-v5.
 *
 * Usage: fshare2fifo [-f FIFO] [-o FILE]
 *   default output: fifo /tmp/h264_high_fifo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define FRAME_COUNTER_OFFSET 0x18
#define DEFAULT_FIFO "/tmp/h264_high_fifo"
#define SCAN_START 0x100          /* skip the small header */
#define SCAN_LIMIT 0x20000        /* stream base is expected before this */
#define POLL_MS 10
#define QUIET_POLLS 3              /* counter stable for N polls = caught up */

static volatile uint8_t *buf;      /* mmap of the shared buffer */
static size_t buf_size;
static size_t stream_base;         /* first NAL position in the ring */
static FILE *out;
static int out_is_fifo;

/* Keep the fifo open for writing without blocking forever when no reader
 * has appeared yet: a helper thread opens it read-only to unblock us. */
static void *unlock_fifo_thread(void *arg)
{
    const char *fifo_name = arg;
    char dummy[1024];
    int fd = open(fifo_name, O_RDONLY);
    if (fd >= 0)
        read(fd, dummy, sizeof(dummy));
    return NULL;
}

static int is_nal_type(uint8_t b)
{
    switch (b & 0x1F) {
    case 1:  /* non-IDR slice */
    case 5:  /* IDR slice */
    case 6:  /* SEI */
    case 7:  /* SPS */
    case 8:  /* PPS */
    case 9:  /* AUD */
        return 1;
    default:
        return 0;
    }
}

/* Find the next NAL start (00 00 01 + plausible type) at or after *pos,
 * wrapping at the end of the buffer. Returns 1 and sets *pos, or 0 if the
 * whole ring was scanned without a hit. */
static int next_nal(size_t *pos)
{
    size_t p = *pos;
    size_t scanned = 0;

    while (scanned < buf_size) {
        if (p + 4 > buf_size) {          /* wrap */
            p = 0;
        }
        if (p + 4 <= buf_size &&
            buf[p] == 0x00 && buf[p + 1] == 0x00 && buf[p + 2] == 0x01 &&
            is_nal_type(buf[p + 3])) {
            *pos = p;
            return 1;
        }
        p++;
        scanned++;
    }
    return 0;
}

static uint32_t frame_counter(void)
{
    uint32_t v;
    memcpy(&v, (const void *)(buf + FRAME_COUNTER_OFFSET), sizeof(v));
    return v;
}

/* Forward frames from the ring while the producer keeps bumping the frame
 * counter. Called repeatedly; pos carries the read position across calls. */
static void drain_round(size_t *pos, uint32_t *last)
{
    int i;

    for (i = 0; i < 64; i++) {
        size_t s = *pos, e;

        if (!next_nal(&s))          /* nothing in the ring */
            return;
        e = s + 3;
        if (!next_nal(&e) || e <= s) /* no complete frame yet */
            return;
        if (fwrite(buf + s, e - s, 1, out) != 1)
            return;
        fflush(out);
        *pos = e;
        if (frame_counter() == *last) /* caught up with the writer */
            return;
        *last = frame_counter();
    }
}

static void drain_forever(void)
{
    size_t pos = stream_base;
    uint32_t last = frame_counter();

    while (1) {
        uint32_t c = frame_counter();
        if (c == last) {
            usleep(POLL_MS * 1000);
            continue;
        }
        last = c;
        drain_round(&pos, &last);
    }
}

int main(int argc, char **argv)
{
    const char *fifo_name = DEFAULT_FIFO;
    const char *out_file = NULL;
    int opt;
    int fd;
    pthread_t unlock_thread;
    struct stat st;

    while ((opt = getopt(argc, argv, "f:o:h")) != -1) {
        switch (opt) {
        case 'f': fifo_name = optarg; break;
        case 'o': out_file = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-f FIFO] [-o FILE]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    fd = open(BUFFER_FILE, O_RDONLY);
    if (fd < 0) {
        perror(BUFFER_FILE);
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    buf_size = (size_t)st.st_size;
    buf = mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* locate the stream base: first NAL after the header */
    stream_base = SCAN_START;
    if (!next_nal(&stream_base)) {
        fprintf(stderr, "no H.264 stream found in %s\n", BUFFER_FILE);
        return 1;
    }
    fprintf(stderr, "stream base at 0x%zX, buffer %zu bytes\n",
            stream_base, buf_size);

    if (out_file) {
        out = fopen(out_file, "wb");
        if (!out) {
            perror(out_file);
            return 1;
        }
        out_is_fifo = 0;
    } else {
        if (mkfifo(fifo_name, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo");
            return 1;
        }
        /* unblock our writer-open in case no reader has connected yet */
        pthread_create(&unlock_thread, NULL, unlock_fifo_thread,
                       (void *)fifo_name);
        out = fopen(fifo_name, "wb");
        if (!out) {
            perror(fifo_name);
            return 1;
        }
        out_is_fifo = 1;
    }

    drain_forever();

    fclose(out);
    munmap((void *)buf, buf_size);
    return 0;
}
