/* ARM1176 VFP "first instruction" probe — run ON the camera.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * probe-vfp (single process) showed every instruction legal once the VFP
 * unit was warmed up with a single-precision op. This one makes a given
 * instruction the process's VERY FIRST VFP use, to test the kernel's
 * lazy-VFP-enable path:
 *
 *   /tmp/probe-vfp2 d   first VFP instr = fldd (double load)
 *   /tmp/probe-vfp2 m   first VFP instr = vldmia {d8-d15} (musl setjmp)
 *   /tmp/probe-vfp2 s   first VFP instr = flds (single, control)
 *
 * Build (WSL): arm-linux-musleabi-gcc -march=armv6 -mfloat-abi=softfp \
 *                  -mfpu=vfp -static -Os -o probe-vfp2 probe-vfp2.c
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>

static double dbuf[16] __attribute__((aligned(16)));
static volatile int sig;

static void on_sig(int n, siginfo_t *si, void *uc)
{
    (void)si;
    ucontext_t *u = uc;
    u->uc_mcontext.arm_pc += 4;   /* skip the faulting instruction */
    sig = n;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    volatile double *p = dbuf;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: %s <d|m|s>\n", argv[0]); return 1; }

    sig = 0;
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    memset(dbuf, 0, sizeof(dbuf));

    switch (argv[1][0]) {
    case 'd':
        __asm__ volatile ("fldd d0, [%0]" :: "r"(p));
        printf("fldd-first %s\n", sig ? "SIGILL" : "OK");
        break;
    case 'm':
        __asm__ volatile ("vldmia %0!, {d8-d15}" :: "r"(p) : "memory");
        printf("vldmia-d8-d15-first %s\n", sig ? "SIGILL" : "OK");
        break;
    case 's':
        __asm__ volatile ("flds s0, [%0]" :: "r"(p));
        printf("flds-first %s\n", sig ? "SIGILL" : "OK");
        break;
    default:
        printf("usage: %s <d|m|s>\n", argv[0]);
        return 1;
    }
    return 0;
}
