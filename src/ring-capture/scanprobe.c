/* scanprobe.c - locate the TRUE write head when the c0 checkpoints are
 * dead (the current raw-mode era): snapshot the ring, wait 300 ms, and
 * diff the live ring against the snapshot - the writer writes
 * contiguously, so the top of the newest changed run IS the write head
 * (the run's end). Reports the head's distance to the header cursors
 * 0x04 / 0x0C / 0x10 and their own movement during the window.
 *
 * Run on the camera:  scanprobe
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define RING      "/dev/shm/fshare_frame_buf"
#define DATA_BASE 0x100

static volatile unsigned char *buf;
static size_t buf_size;
static unsigned char *snap;

static uint32_t rd32(uint32_t p)
{
    if (p + 3 >= buf_size)
        return 0;
    return (uint32_t)buf[p] | ((uint32_t)buf[p + 1] << 8) |
           ((uint32_t)buf[p + 2] << 16) | ((uint32_t)buf[p + 3] << 24);
}

static int ring_delta(uint32_t a, uint32_t b)
{
    int d = (int)(b - a);
    if (d < -(int)(buf_size / 2))
        d += (int)buf_size;
    if (d > (int)(buf_size / 2))
        d -= (int)buf_size;
    return d;
}

int main(void)
{
    int fd;
    struct stat st;
    uint32_t p;
    struct { uint32_t s, e; } runs[4];
    int nruns = 0, in_run = 0, i;
    uint32_t head = 0;
    uint32_t a04a, a0ca, a10a, a04b, a0cb, a10b;

    fd = open(RING, O_RDONLY);
    if (fd < 0) {
        perror(RING);
        return 1;
    }
    fstat(fd, &st);
    buf_size = (size_t)st.st_size;
    buf = mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    snap = malloc(buf_size);
    if (!snap) {
        perror("malloc");
        return 1;
    }
    printf("buf_size=%d\n", (int)buf_size);

    memcpy(snap, (const void *)buf, buf_size);
    a04a = rd32(4); a0ca = rd32(12); a10a = rd32(16);
    usleep(300 * 1000);
    a04b = rd32(4); a0cb = rd32(12); a10b = rd32(16);

    /* collect the changed runs [s, e) */
    for (p = DATA_BASE; p < buf_size; p++) {
        int ch = (buf[p] != snap[p]);
        if (ch && !in_run) {
            runs[nruns].s = p;
            in_run = 1;
        } else if (!ch && in_run) {
            runs[nruns].e = p;
            nruns++;
            in_run = 0;
            if (nruns == 4)
                break;
        }
    }
    if (in_run && nruns < 4) {
        runs[nruns].e = (uint32_t)buf_size;
        nruns++;
    }

    printf("cursor window: 04: %u -> %u (%d)   0c: %u -> %u (%d)   10: %u -> %u (%d)\n",
           a04a, a04b, (int)(a04b - a04a),
           a0ca, a0cb, (int)(a0cb - a0ca),
           a10a, a10b, (int)(a10b - a10a));
    if (nruns == 0) {
        printf("no changed bytes - the writer is idle\n");
        return 0;
    }
    for (i = 0; i < nruns; i++)
        printf("changed run %d: [%u, %u)  %d B\n",
               i, runs[i].s, runs[i].e, (int)(runs[i].e - runs[i].s));

    /* the head = the end of the newest run: normally the last run; when
     * the last run touches the buffer end and an early run starts right
     * after DATA_BASE, the writer wrapped and the early run is newest */
    head = runs[nruns - 1].e;
    if (runs[nruns - 1].e == buf_size && nruns >= 2 &&
        runs[0].s < DATA_BASE + 4096)
        head = runs[0].e;

    printf("head=%u\n", head);
    printf("head - 04 = %d\n", ring_delta(a04b, head));
    printf("head - 0c = %d\n", ring_delta(a0cb, head));
    printf("head - 10 = %d\n", ring_delta(a10b, head));
    printf("bytes at head:\n  ");
    for (i = 0; i < 40; i++)
        printf("%02x ", buf[head + i]);
    printf("\n");
    free(snap);
    return 0;
}
