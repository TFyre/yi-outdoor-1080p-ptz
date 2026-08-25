/* endprobe.c - what sits AT the committed ends? Each time the writer
 * publishes a new committed end (hdr 0x0C or 0x10 advances), dump the
 * bytes at that position. If the ends are frame-aligned they land on a
 * record header (magic 0x6a8c, type 0x0400 hi-res / 0x0800 low-res /
 * 0x0100 audio) or a NAL start code - and the cursor-to-stream mapping
 * falls out: the cursor whose ends land on 0x0400 records carries the
 * hi-res stream.
 *
 * Run on the camera:  endprobe [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#define RING      "/dev/shm/fshare_frame_buf"
#define DATA_BASE 0x100
#define NBYTES    40

static volatile unsigned char *buf;
static size_t buf_size;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* wrap-aware read of n bytes at pos into out */
static void ring_read(unsigned char *out, uint32_t pos, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        uint32_t p = pos + (uint32_t)i;
        if (p >= buf_size)
            p = DATA_BASE + (p - buf_size);
        out[i] = buf[p];
    }
}

/* b - a in ring space */
static int ring_delta(uint32_t a, uint32_t b)
{
    int d = (int)(b - a);
    if (d < -(int)(buf_size / 2))
        d += (int)buf_size;
    if (d > (int)(buf_size / 2))
        d -= (int)buf_size;
    return d;
}

static void classify(const unsigned char *b)
{
    uint32_t magic = b[6] | (b[7] << 8) | (b[8] << 16) | (b[9] << 24);
    int type = b[18] | (b[19] << 8);
    int i;

    if ((magic & 0xffff0000) == 0x6a8c0000)
        printf("    RECORD magic=0x%08x type=0x%04x", magic, type);
    else if (b[0] == 0 && b[1] == 0 && b[2] == 1)
        printf("    SC 00 00 01");
    else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1)
        printf("    SC 00 00 00 01");
    else
        printf("    (no record/SC)");
    printf("\n    bytes: ");
    for (i = 0; i < NBYTES; i++)
        printf("%02x ", b[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int fd;
    struct stat st;
    uint32_t prev0c = 0, prev10 = 0;
    int have = 0;
    long long t0;

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
    printf("buf_size=%d\n", (int)buf_size);

    t0 = now_ms();
    while (now_ms() - t0 < (long long)secs * 1000) {
        uint32_t a04, a0c, a10;
        long long t = now_ms() - t0;
        unsigned char b[NBYTES];

        memcpy(&a04, (const void *)(buf + 4), 4);
        memcpy(&a0c, (const void *)(buf + 12), 4);
        memcpy(&a10, (const void *)(buf + 16), 4);

        if (have && ring_delta(prev0c, a0c) > 512) {
            printf("t=%5lld ms 0x0C: %u -> %u (d=+%d)  04=%u\n",
                   t, prev0c, a0c, ring_delta(prev0c, a0c), a04);
            ring_read(b, a0c, NBYTES);
            classify(b);
        }
        if (have && ring_delta(prev10, a10) > 512) {
            printf("t=%5lld ms 0x10: %u -> %u (d=+%d)  04=%u\n",
                   t, prev10, a10, ring_delta(prev10, a10), a04);
            ring_read(b, a10, NBYTES);
            classify(b);
        }
        prev0c = a0c;
        prev10 = a10;
        have = 1;
        usleep(10000);
    }
    return 0;
}
