/* slotwatch.c - strictly READ-ONLY observation of the fshare reader slot
 * table, to answer questions the tserver disassembly cannot:
 *
 *   - do the stock readers ever actually park (slot.waiting -> 1)?
 *     If they never park, the writer never needs to post a notify
 *     semaphore, and a v2 reader can ignore the semaphores entirely.
 *   - which slots are occupied (filter != 0), and do free slots stay
 *     all-zero?
 *   - how far behind the writer does each reader run?
 *
 * Maps the ring PROT_READ only. Takes no lock, writes nothing, posts
 * nothing. Safe to run against the live camera.
 *
 *   slotwatch [seconds]      (default 10)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#define RING     "/dev/shm/fshare_frame_buf"
#define RING_SZ  1786156u
#define SLOT_OFF 0x1Cu
#define NSLOT    17
#define SAMPLE_US 2000

struct slot { uint32_t pending, waiting, cursor; uint16_t filter, pad; };

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int fd, k;
    volatile unsigned char *m;
    long long t0;
    uint32_t waits[NSLOT], pend_edges[NSLOT], samples = 0;
    uint32_t first_cur[NSLOT], last_cur[NSLOT];
    uint16_t filt[NSLOT];
    struct slot prev[NSLOT];

    fd = open(RING, O_RDONLY);
    if (fd < 0) { perror(RING); return 1; }
    m = mmap(NULL, RING_SZ, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    close(fd);

    memset(waits, 0, sizeof waits);
    memset(pend_edges, 0, sizeof pend_edges);
    for (k = 0; k < NSLOT; k++) {
        memcpy(&prev[k], (const void *)(m + SLOT_OFF + 16 * k), 16);
        first_cur[k] = prev[k].cursor;
        filt[k] = prev[k].filter;
    }

    printf("watching %d slots for %d s (sample %d us), read-only\n",
           NSLOT, secs, SAMPLE_US);
    t0 = now_ms();
    while (now_ms() - t0 < (long long)secs * 1000) {
        for (k = 0; k < NSLOT; k++) {
            struct slot s;
            memcpy(&s, (const void *)(m + SLOT_OFF + 16 * k), 16);
            if (s.waiting)                       waits[k]++;
            if (s.pending && !prev[k].pending)   pend_edges[k]++;
            if (s.filter)                        filt[k] = s.filter;
            last_cur[k] = s.cursor;
            prev[k] = s;
        }
        samples++;
        usleep(SAMPLE_US);
    }

    printf("samples=%u  newest_seq=%u  tail=%u  valid=%u\n", samples,
           *(volatile uint32_t *)(m + 0x18), *(volatile uint32_t *)(m + 0x10),
           *(volatile uint32_t *)(m + 0x04));
    printf("slot  filter  cursor_delta  waiting_seen  pending_edges  state\n");
    for (k = 0; k < NSLOT; k++) {
        if (!filt[k] && !last_cur[k] && !waits[k])
            continue;
        printf("%4d  0x%04x  %12u  %12u  %13u  %s\n", k, filt[k],
               last_cur[k] - first_cur[k], waits[k], pend_edges[k],
               filt[k] ? "occupied" : "stale/free");
    }
    munmap((void *)m, RING_SZ);
    return 0;
}
