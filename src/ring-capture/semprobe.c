/* semprobe.c - observe the app's reader-slot semaphores without
 * SPDX-License-Identifier: GPL-3.0-or-later
 * disturbing them: timed-wait on each notify semaphore and report
 * which slots fire and when. The firing pattern reveals the writer's
 * fan-out (which consumer slots the app's own readers hold).
 *
 * Run on the camera:  semprobe            (probes slots 0..16, 2 s each)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    char name[64];
    int slot;
    long long t0 = now_ms();

    for (slot = 0; slot < 17; slot++) {
        sem_t *s;
        struct timespec ts;
        int got = 0;
        long long start = now_ms();

        snprintf(name, sizeof(name), "/sem.fshare_read_notify_%d", slot);
        s = sem_open(name, 0);
        if (s == SEM_FAILED) {
            printf("slot %2d: sem_open failed (%s)\n", slot, strerror(errno));
            continue;
        }
        while (now_ms() - start < 2000) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            if (sem_timedwait(s, &ts) == 0) {
                got++;
            } else if (errno != ETIMEDOUT) {
                break;
            }
        }
        sem_close(s);
        printf("slot %2d: %d notifies in 2 s (t=%lld ms)\n",
               slot, got, now_ms() - t0);
    }
    return 0;
}
