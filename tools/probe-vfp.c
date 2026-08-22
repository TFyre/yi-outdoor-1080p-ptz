/* ARM1176 (VFPv2) instruction-set probe — run ON the camera.
 *
 * The musl arm port's setjmp/longjmp saves/restores VFP double registers
 * with FLDMD/FSTMD ({d8-d15} multiples) when HWCAP_VFP is set, and the
 * armv6 purity scan flags those as "VFPv3-only". Whether they are legal
 * here is an empirical question: build with the camera toolchain, run it,
 * and see which probes print OK.
 *
 * Build (WSL): arm-linux-musleabi-gcc -march=armv6 -mfloat-abi=softfp \
 *                  -mfpu=vfp -static -Os -o probe-vfp probe-vfp.c
 * Run: scp to /tmp on the camera, then /tmp/probe-vfp
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>

static double dbuf[16] __attribute__((aligned(16)));
static volatile int sig;

/* Skip the faulting instruction by advancing the saved PC. Returning from
 * a SIGILL handler normally re-executes the faulting instruction, which
 * would loop forever — and we want a report, not a busy hang. */
static void on_sig(int n, siginfo_t *si, void *uc)
{
    (void)si;
    ucontext_t *u = uc;
    u->uc_mcontext.arm_pc += 4;
    sig = n;
}

/* Run one probe, report SIGILL vs OK. Each probe gets fresh buffers so a
 * faulted write cannot corrupt the next test. */
static void probe(const char *name, void (*fn)(double *))
{
    volatile double *p = dbuf;
    struct sigaction sa;

    sig = 0;
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    memset(dbuf, 0, sizeof(dbuf));
    fn((double *)p);
    printf("%-20s %s\n", name, sig ? "SIGILL" : "OK");
}

/* baseline: single-precision load/store (VFPv1 already had this) */
static void p_vldr_s(double *p)
{
    __asm__ volatile ("flds s0, [%0]" :: "r"(p));
    __asm__ volatile ("fsts s0, [%0]" :: "r"(p) : "memory");
}

/* single-precision arithmetic */
static void p_vadd_f32(double *p)
{
    __asm__ volatile ("flds s0, [%0]" :: "r"(p));
    __asm__ volatile ("fadds s1, s0, s0" ::: "memory");
}

/* double-register load/store */
static void p_vldr(double *p)
{
    __asm__ volatile ("fldd d0, [%0]" :: "r"(p));
    __asm__ volatile ("fstd d0, [%0]" :: "r"(p) : "memory");
}

/* the musl setjmp pattern: FLDMD/FSTMD {d8-d15} */
static void p_vldm_d8_15(double *p)
{
    __asm__ volatile ("vldmia %0!, {d8-d15}" :: "r"(p) : "memory");
    __asm__ volatile ("vstmia %0!, {d8-d15}" :: "r"(p) : "memory");
}

/* single-register multiples (FSTMS/FLDMS) */
static void p_vldm_s(double *p)
{
    __asm__ volatile ("fldmias %0!, {s0-s15}" :: "r"(p) : "memory");
    __asm__ volatile ("fstmias %0!, {s0-s15}" :: "r"(p) : "memory");
}

/* known-bad control: ARMv7 movw, embedded as a raw word so the
 * assembler's .arch gate cannot reject the mnemonic */
static void p_movw(double *p)
{
    (void)p;
    __asm__ volatile (".word 0xE3001234\n\t" ::: "r0");
}

/* double-precision arithmetic */
static void p_vadd_f64(double *p)
{
    __asm__ volatile ("fldd d0, [%0]" :: "r"(p));
    __asm__ volatile ("faddd d1, d0, d0" ::: "memory");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("ARM1176 VFPv2 probe\n");
    probe("flds/fsts s0", p_vldr_s);
    probe("fadds", p_vadd_f32);
    probe("fldd/fstd d0", p_vldr);
    probe("vldm/vstm d8-d15", p_vldm_d8_15);
    probe("fldm/fstm s0-s15", p_vldm_s);
    probe("movw (control)", p_movw);
    probe("faddd", p_vadd_f64);
    return 0;
}
