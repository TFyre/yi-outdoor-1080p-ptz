/* slotprobe.c - empirically map the fshare slot protocol, strictly
 * SPDX-License-Identifier: GPL-3.0-or-later
 * read-only. Answers, without touching any lock or the app's data:
 *
 *  A: what else lives in /dev/shm besides the semaphores
 *     (registration files?)
 *  B: each semaphore's backing-file words (counter / futex waiters) -
 *     which notify slots have a WAITER right now, i.e. which slots
 *     the app's own readers hold
 *  C: whether posts ever target waiter-less slots (trywait polling;
 *     slots with an app waiter are skipped so we never steal a post)
 *  D: which ring-header fields move and how (250 ms samples)
 *
 * Run on the camera:  slotprobe [seconds [ms]]
 *   ms > 0 enables part E: 10 ms-scale sampling of the 0x04 header
 *   field plus the first three notify words, printing the context
 *   around every backward 0x04 move.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define RING   "/dev/shm/fshare_frame_buf"
#define NSLOTS 17
#define E_SECS 6

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the futex word of a semaphore's backing file (-1 on failure) */
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

/* read a semaphore's backing file: word0 = the counter, word1 = the
 * futex waiters count (uClibc sem_t). Returns word1, or -1. */
static int dump_sem_file(const char *path, const char *label)
{
    unsigned char raw[32];
    int fd, n, w0 = -1, w1 = -1;
    struct stat st;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("%s: open failed (%s)\n", label, strerror(errno));
        return -1;
    }
    fstat(fd, &st);
    n = (int)read(fd, raw, sizeof(raw));
    close(fd);
    if (n >= 8) {
        memcpy(&w0, raw, 4);
        memcpy(&w1, raw + 4, 4);
    }
    printf("%s(%d B): word0=%d (0x%08x)  word1=%d (0x%08x)\n",
           label, (int)st.st_size, w0, (unsigned)w0, w1, (unsigned)w1);
    return w1;
}

static void part_e(int ms);

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int waiters[NSLOTS];
    int k;

    printf("== A: /dev/shm contents ==\n");
    fflush(stdout);
    system("ls -la /dev/shm");

    printf("== B: semaphore backing words ==\n");
    dump_sem_file("/dev/shm/sem.fshare_read_lock", "read_lock ");
    dump_sem_file("/dev/shm/sem.fshare_write_lock", "write_lock");
    for (k = 0; k < NSLOTS; k++) {
        char path[64], label[16];
        snprintf(path, sizeof(path), "/dev/shm/sem.fshare_read_notify_%d", k);
        snprintf(label, sizeof(label), "notify_%2d ", k);
        waiters[k] = dump_sem_file(path, label) > 0;
    }

    {
        sem_t *sems[NSLOTS];
        int counts[NSLOTS];
        long long t0, t;

        memset(counts, 0, sizeof(counts));
        printf("== C: trywait poll for %d s (waiter-held slots skipped) ==\n",
               secs);
        for (k = 0; k < NSLOTS; k++) {
            char name[64];
            snprintf(name, sizeof(name), "/sem.fshare_read_notify_%d", k);
            sems[k] = sem_open(name, 0);
            if (sems[k] == SEM_FAILED) {
                printf("notify_%2d: sem_open failed (%s)\n", k,
                       strerror(errno));
                sems[k] = NULL;
            }
        }
        t0 = now_ms();
        while ((t = now_ms() - t0) < (long long)secs * 1000) {
            for (k = 0; k < NSLOTS; k++) {
                if (!sems[k] || waiters[k])
                    continue;       /* never steal the app's posts */
                if (sem_trywait(sems[k]) == 0)
                    counts[k]++;
            }
            usleep(20000);
        }
        for (k = 0; k < NSLOTS; k++) {
            int v = -1;
            if (sems[k]) {
                sem_getvalue(sems[k], &v);
                sem_close(sems[k]);
            }
            printf("notify_%2d: %d posts observed, value now %d%s\n",
                   k, counts[k], v, waiters[k] ? "  [app waiter]" : "");
        }
    }

    {
        int fd;
        struct stat st;
        volatile unsigned char *m;
        uint32_t prev[0x100 / 4];
        long long t0, t;

        printf("== D: ring header movement (%d s, 250 ms samples) ==\n", secs);
        fd = open(RING, O_RDONLY);
        if (fd < 0) {
            perror(RING);
            return 1;
        }
        fstat(fd, &st);
        m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return 1;
        }
        printf("buf_size=%d\n", (int)st.st_size);
        memcpy(prev, (const void *)m, sizeof(prev));
        t0 = now_ms();
        while ((t = now_ms() - t0) < (long long)secs * 1000) {
            uint32_t cur[0x100 / 4];
            int i;
            usleep(250000);
            memcpy(cur, (const void *)m, sizeof(cur));
            for (i = 0; i < 0x100 / 4; i++) {
                if (cur[i] != prev[i])
                    printf("  t=%4lld ms  hdr[0x%02X]: %u -> %u\n",
                           t, i * 4, prev[i], cur[i]);
            }
            memcpy(prev, cur, sizeof(cur));
        }
        munmap((void *)m, (size_t)st.st_size);
        close(fd);
    }

    if (argc > 2 && atoi(argv[2]) > 0)
        part_e(atoi(argv[2]));
    return 0;
}

/* part E: high-rate sampling of the 0x04 field plus the first three
 * notify words, printing the context around every backward 0x04 move.
 * Resolves whether the periodic backward moves are ring wraps (the
 * landing position is the region base) or true jumps, and whether the
 * notify semaphores change at those moments. */
static void part_e(int ms)
{
    int fd;
    struct stat st;
    volatile unsigned char *m;
    long long t0;
    uint32_t prev04 = 0;
    int first = 1;
    struct {
        long long t;
        uint32_t a04, a0c, a10, a14, a18;
        int n0, n1, n2;
    } ring[4];
    int have = 0, rn = 0, k;

    printf("== E: %d ms sampling of 0x04 + notify words (%d s) ==\n",
           ms, E_SECS);
    fd = open(RING, O_RDONLY);
    if (fd < 0) {
        perror(RING);
        return;
    }
    fstat(fd, &st);
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return;
    }

    t0 = now_ms();
    while (now_ms() - t0 < (long long)E_SECS * 1000) {
        uint32_t a04, a0c, a10, a14, a18;
        long long cur = now_ms() - t0;

        memcpy(&a04, (const void *)(m + 4), 4);
        memcpy(&a0c, (const void *)(m + 12), 4);
        memcpy(&a10, (const void *)(m + 16), 4);
        memcpy(&a14, (const void *)(m + 20), 4);
        memcpy(&a18, (const void *)(m + 24), 4);

        ring[rn].t = cur;
        ring[rn].a04 = a04;
        ring[rn].a0c = a0c;
        ring[rn].a10 = a10;
        ring[rn].a14 = a14;
        ring[rn].a18 = a18;
        ring[rn].n0 = sem_word0("/dev/shm/sem.fshare_read_notify_0");
        ring[rn].n1 = sem_word0("/dev/shm/sem.fshare_read_notify_1");
        ring[rn].n2 = sem_word0("/dev/shm/sem.fshare_read_notify_2");
        rn = (rn + 1) % 4;
        if (have < 4)
            have++;

        if (!first && a04 < prev04 && prev04 - a04 > 1024) {
            printf("-- backward 0x04 move at t=%lld ms: %u -> %u --\n",
                   cur, prev04, a04);
            for (k = 0; k < have; k++) {
                int i = (rn - have + k + 4) % 4;
                printf("   t=%5lld 04=%u 0c=%u 10=%u 14=%u 18=%u"
                       " n0=%d n1=%d n2=%d\n",
                       ring[i].t, ring[i].a04, ring[i].a0c, ring[i].a10,
                       ring[i].a14, ring[i].a18,
                       ring[i].n0, ring[i].n1, ring[i].n2);
            }
        }
        prev04 = a04;
        first = 0;
        usleep((useconds_t)ms * 1000);
    }
    munmap((void *)m, (size_t)st.st_size);
    close(fd);
}
