/*
 * fshare2fifo - forward the H.264 stream from the YI FH8626V100 shared
 * frame buffer (/dev/shm/fshare_frame_buf) to a fifo or file.
 *
 * The stock app (rmm) continuously writes the encoded H.264 stream into
 * the shared buffer and bumps a frame counter at offset 0x18. This tool
 * watches the counter and forwards new frames as they are written.
 *
 * Before forwarding anything it waits for a complete SPS->PPS->IDR chain
 * in the ring, so clients joining the stream decode a clean picture from
 * their very first frame instead of mid-GOP artifacts.
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
 * has appeared yet: a helper thread opens it read-only to unblock us, then
 * parks with the fd open (a fifo writer with no reader gets EPIPE).
 *
 * NOTE: it must never read() from the fifo — an early version did and
 * silently stole the first 1024 bytes of the stream (the SPS+PPS+IDR
 * head), which made every client join mid-GOP. */
static void *unlock_fifo_thread(void *arg)
{
    const char *fifo_name = arg;
    int fd = open(fifo_name, O_RDONLY);
    if (fd >= 0) {
        while (1)
            pause();               /* hold the fd open; never consume data */
    }
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

/* Find a decodable start point: the most recent complete SPS->PPS->IDR
 * chain in the ring. Emitting from anywhere else means clients join
 * mid-GOP and show decoder artifacts until the next keyframe; starting
 * at the SPS of a complete chain gives every new client a clean picture
 * from its first frame. SEI/AUD NALs may appear inside the chain.
 * Returns 1 and sets *out, or 0 if no complete chain is in the ring. */
static int find_idr_start(size_t *out)
{
    size_t p = SCAN_START;
    size_t scanned = 0;
    size_t sps_pos = 0;
    size_t best = 0;
    int found_chain = 0;
    int have_sps = 0;      /* 0: none, 1: SPS seen, 2: SPS+PPS seen */

    while (scanned < buf_size) {
        int found = 0;
        while (scanned < buf_size) {          /* next start code, wrapping */
            if (p + 4 > buf_size)
                p = 0;
            if (p + 4 <= buf_size &&
                buf[p] == 0x00 && buf[p + 1] == 0x00 && buf[p + 2] == 0x01 &&
                is_nal_type(buf[p + 3])) {
                found = 1;
                break;
            }
            p++;
            scanned++;
        }
        if (!found)
            break;

        switch (buf[p + 3] & 0x1F) {
        case 7:                             /* SPS restarts the chain */
            sps_pos = p;
            have_sps = 1;
            break;
        case 8:                             /* PPS follows the SPS */
            if (have_sps == 1)
                have_sps = 2;
            break;
        case 5:                             /* IDR completes the chain */
            if (have_sps == 2) {
                best = sps_pos;
                found_chain = 1;
                have_sps = 0;
            }
            break;
        case 6: case 9:                     /* SEI/AUD: legal inside */
            break;
        default:                            /* anything else breaks it */
            have_sps = 0;
            break;
        }
        p += 4;
        scanned += 4;
    }
    if (!found_chain)
        return 0;
    *out = best;
    return 1;
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
    int i;
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

    /* The fifo is opened before the start-point wait below, so a server
     * can attach immediately and just see no bytes until the gate passes. */
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

    /* Wait for a decodable start point: a complete SPS->PPS->IDR chain.
     * Emitting from anywhere else means every client joins mid-GOP and
     * shows decoder artifacts until the next keyframe. The ring holds
     * ~1.7 MB (several seconds of video), so once the app is writing, a
     * chain appears within one GOP interval; give it up to ~30 s. */
    stream_base = SCAN_START;
    for (i = 0; !find_idr_start(&stream_base); i++) {
        if (i == 0)
            fprintf(stderr, "waiting for SPS/PPS/IDR chain...");
        if (i >= 600) {
            fprintf(stderr, " none found in %s\n", BUFFER_FILE);
            fclose(out);
            munmap((void *)buf, buf_size);
            return 1;
        }
        usleep(50 * 1000);
    }
    fprintf(stderr, "IDR start at 0x%zX, buffer %zu bytes\n",
            stream_base, buf_size);

    drain_forever();

    fclose(out);
    munmap((void *)buf, buf_size);
    return 0;
}
