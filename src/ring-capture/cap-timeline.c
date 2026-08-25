/* cap-timeline.c - capture the RESERVATION TIMELINE plus a
 * tear-checked ring snapshot, for offline validation of the
 * region-emission reader (fshare2fifo v2).
 *
 * For N seconds: log the header cursors every 20 ms
 * ("ms 04 0c 10 14 18 n0 n1 n2" lines to /tmp/timeline.txt), then
 * snapshot the whole ring to /tmp/ring-cap.bin with a bounded
 * double-read tear check (equal reads = the ring was stable during
 * the copy; the writer laps a single 1.79 MB read at burst rates).
 *
 * Run on the camera:  cap-timeline [seconds]
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

#define RING "/dev/shm/fshare_frame_buf"

static volatile unsigned char *buf;
static size_t buf_size;

static uint32_t rd32(uint32_t p)
{
    if (p + 3 >= buf_size)
        return 0;
    return (uint32_t)buf[p] | ((uint32_t)buf[p + 1] << 8) |
           ((uint32_t)buf[p + 2] << 16) | ((uint32_t)buf[p + 3] << 24);
}

static int sem_word0(const char *path)
{
    unsigned char raw[4];
    int fd, n, w = -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = (int)read(fd, raw, sizeof(raw));
    close(fd);
    if (n >= 4)
        memcpy(&w, raw, 4);
    return w;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    int fd;
    struct stat st;
    FILE *tl;
    unsigned char *snap1, *snap2;
    long long t0;
    int tries;

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

    tl = fopen("/tmp/timeline.txt", "w");
    if (!tl) {
        perror("/tmp/timeline.txt");
        return 1;
    }
    fprintf(tl, "# ms 04 0c 10 14 18 n0 n1 n2 (buf_size=%d)\n", (int)buf_size);
    fflush(tl);

    t0 = now_ms();
    while (now_ms() - t0 < (long long)secs * 1000) {
        long long t = now_ms() - t0;
        fprintf(tl, "%lld %u %u %u %u %u %d %d %d\n",
                t, rd32(4), rd32(12), rd32(16), rd32(20), rd32(24),
                sem_word0("/dev/shm/sem.fshare_read_notify_0"),
                sem_word0("/dev/shm/sem.fshare_read_notify_1"),
                sem_word0("/dev/shm/sem.fshare_read_notify_2"));
        fflush(tl);
        usleep(20 * 1000);
    }
    fclose(tl);

    /* tear-checked snapshot: two full reads must agree */
    snap1 = malloc(buf_size);
    snap2 = malloc(buf_size);
    if (!snap1 || !snap2) {
        perror("malloc");
        return 1;
    }
    for (tries = 0; tries < 10; tries++) {
        memcpy(snap1, (const void *)buf, buf_size);
        memcpy(snap2, (const void *)buf, buf_size);
        if (memcmp(snap1, snap2, buf_size) == 0)
            break;
        usleep(100 * 1000);
    }
    {
        FILE *out = fopen("/tmp/ring-cap.bin", "wb");
        if (!out) {
            perror("/tmp/ring-cap.bin");
            return 1;
        }
        fwrite(snap1, 1, buf_size, out);
        fclose(out);
    }
    printf("timeline: /tmp/timeline.txt (%d s)\n", secs);
    printf("snapshot: /tmp/ring-cap.bin (tear-check %s after %d tries)\n",
           tries < 10 ? "clean" : "FAILED", tries);
    return 0;
}
