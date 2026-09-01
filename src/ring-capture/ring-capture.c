/* ring-capture.c - the REFERENCE reader: record the app's raw output
 * SPDX-License-Identifier: GPL-3.0-or-later
 * from the ring for N seconds, in the true write order (wraps included).
 *
 * Unlike fshare2fifo this does no gating, joining, or stream filtering -
 * it tracks the write head by diffing the ring every 250 ms and appends
 * every new byte to the output file. The result is the ground truth of
 * what the app wrote, which the walk's emission can then be measured
 * against (missing frames, ordering, torn regions).
 *
 * The head tracking mirrors fshare2fifo's diff sampler: a shadow copy
 * of the scanned window, the top of the newest changed run IS the write
 * head. The window is 256 KB - enough for one tick at the measured peak
 * write rate (~1 MB/s) while keeping the anonymous memory tiny (the
 * camera's OOM killer has SIGKILLed bigger processes).
 *
 * Build: tools/build-armv6.sh ringcapture   (or the same toolchain)
 * Run on the camera:  ring-capture /tmp/cap.bin 60
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#define DATA_BASE 0x100
#define DIFF_WIN (256u * 1024)
#define BUFFER_FILE "/dev/shm/fshare_frame_buf"

static volatile unsigned char *buf;
static size_t buf_size;
static unsigned char *shadow;

static void *diff_thread(void *arg)
{
    uint32_t base = DATA_BASE;
    FILE *out = (FILE *)arg;
    (void)arg;

    /* seed the shadow with the window's current content */
    memcpy(shadow, buf + DATA_BASE, DIFF_WIN);

    while (1) {
        uint32_t q = base;
        size_t left = DIFF_WIN;
        size_t sdx = 0;
        uint32_t run1 = 0;
        int run = 0;
        size_t i;

        usleep(250 * 1000);

        while (left > 0) {
            size_t n;
            if (q >= buf_size)
                q = 0;
            n = buf_size - q;
            if (n > left)
                n = left;
            if (q < DATA_BASE) {
                size_t skip = DATA_BASE - q;
                if (skip > n)
                    skip = n;
                q += (uint32_t)skip;
                sdx += skip;
                left -= skip;
                continue;
            }
            for (i = 0; i < n; i++) {
                if (buf[q + i] != shadow[sdx + i]) {
                    run = 1;
                    run1 = q + i;
                }
            }
            memcpy(shadow + sdx, (const void *)(buf + q), n);
            q += (uint32_t)n;
            sdx += n;
            left -= n;
        }

        if (run) {
            /* append the new bytes [base, run1+1) in order */
            uint32_t p = base;
            uint32_t end = (run1 + 1 >= buf_size) ? buf_size : run1 + 1;
            while (p != end) {
                size_t chunk = end - p;
                if (chunk > 65536)
                    chunk = 65536;
                fwrite((const void *)(buf + p), 1, chunk, out);
                p += (uint32_t)chunk;
            }
            fflush(out);
            base = (run1 + 1 >= buf_size) ? DATA_BASE : run1 + 1;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *out_name = argc > 1 ? argv[1] : "/tmp/ringcap.bin";
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    int fd;
    struct stat st;
    FILE *out;

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
    shadow = malloc(DIFF_WIN);
    if (!shadow) {
        perror("malloc");
        return 1;
    }
    out = fopen(out_name, "wb");
    if (!out) {
        perror(out_name);
        return 1;
    }

    {
        pthread_t tid;
        pthread_create(&tid, NULL, diff_thread, out);
    }

    sleep(secs);
    fclose(out);
    fprintf(stderr, "ring-capture: wrote %s (%d s)\n", out_name, secs);
    return 0;
}
