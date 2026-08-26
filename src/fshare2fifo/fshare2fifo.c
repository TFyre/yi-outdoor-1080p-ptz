/*
 * fshare2fifo v2 - a PROPER reader of the fshare shared-frame protocol
 * (reverse-engineered from tserver/rmm, analysis/fshare-protocol.md).
 *
 * The ring is NOT a byte stream that needs re-synchronising: it is a
 * sequence-numbered record log with a published tail (0x10), a
 * published valid-byte count (0x04), a 17-entry reader slot table
 * (0x1C + 16*k), and per-slot pending/waiting flags. A registered
 * reader filters records by class (0x0400 = the 1920x1088 stream),
 * advances its slot cursor by sequence number, and sleeps between
 * deliveries. There is nothing to scan, no lap to detect, and no tear
 * possible: while any reader holds read_lock the writer is blocked
 * (first-reader rule on write_lock).
 *
 * v2 therefore has no diff sampler, no shadow window, no start-code
 * ring scan, no head estimation, no MAX_LAG and no re-join machinery.
 * The notify semaphore is skipped entirely (musl's sem_t vs the
 * camera's uClibc sem_t is a hazard) - slot[K].pending is polled on a
 * 10 ms timer; correctness never depends on the semaphore, only
 * wakeup latency does.
 *
 * Record header (26 bytes): [0] len u32 (payload incl. extras prefix)
 * [4] seq u32 (+1 per record, ring-wide) [8] magic u32 [12] cookie
 * [16] ts u32 [20] type u16 [22] chain-part u16 [24] extra u16.
 * Types: 0x0400 hi-res frame, 0x0422 hi SPS#1, 0x0401 hi SPS#2,
 * 0x0404 hi PPS (0x08xx = low-res, 0x0100 = audio - filtered out).
 * The payload carries a 4-byte start code after a 0/5/6-byte extras
 * prefix (type bit 0x20 set when a prefix is present).
 *
 * The chain per GOP: 0x0422 -> 0x0404 -> 0x0401 -> 0x0400(IDR) ->
 * 0x0400(P slices). Join: the cursor starts at the newest 0x0422 so
 * every client's first bytes are a complete SPS->PPS->IDR chain.
 *
 * Usage: fshare2fifo [-f FIFO] [-o FILE] [-n] [-s SLOT]
 *   default output: fifo /tmp/h264_high_fifo
 *   -s: reader slot (default: first free of 10..14)
 *   -n: one-shot - dump up to 256 records, then exit (captures)
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
#include <sys/ioctl.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
/* The mapping: 300-byte header + a 0x1B4000-byte record ring (the size
 * is hardcoded in the app's client - fstat on the live file agrees). */
#define DATA_OFF 300u
#define DATA_SZ  0x1B4000u
#define NSLOT    17
#define HDR_COUNT 0x00           /* active reader count (rw-lock state) */
#define HDR_VALID 0x04           /* valid bytes in the ring */
#define HDR_HEAD  0x0C           /* write head = (tail + valid) mod DATA_SZ */
#define HDR_TAIL  0x10           /* tail: oldest surviving record */
#define HDR_NOW   0x14           /* newest record's timestamp */
#define HDR_SEQ   0x18           /* newest record's sequence number */
#define SLOT_OFF(k) (0x1Cu + 16u * (k))
#define FILTER_HIRES 0x0400u
#define POLL_MS 10
#define MAXPAY  (512u * 1024)    /* the 0x0401 record carries the whole
                                    GOP head (SPS+PPS+zeros+IDR): 78 KB
                                    calm, 300-500 KB at IR-noise rates */
#define NAL_BUF_MAX 256          /* SPS/PPS are tiny */
#define DEFAULT_FIFO "/tmp/h264_high_fifo"

static volatile uint8_t *buf;      /* mmap of the shared buffer */
static size_t buf_size;
static int slot_k = -1;            /* our reader slot */
static uint32_t slot_cursor;       /* mirrored slot[K].cursor */
static FILE *out;                  /* file output (-o FILE) */
static int out_fd = -1;            /* fifo output, non-blocking */
static size_t g_pipe_size = 256 * 1024;
static volatile int stop_flag;

/* ---- diagnostics ---- */
static int f2f_trace(void)  { return getenv("F2F_TRACE") != NULL; }
static int f2f_agelog(void) { return getenv("F2F_AGELOG") != NULL; }
static volatile unsigned g_nals, g_bytes, g_drops, g_block_ticks;
static volatile unsigned g_max_block;

/* ---- header and slot access ---- */
static uint32_t hdr_u32(unsigned off)
{
    uint32_t v;
    memcpy(&v, (const void *)(buf + off), 4);
    return v;
}

static void hdr_wr_u32(unsigned off, uint32_t v)
{
    memcpy((void *)(buf + off), &v, 4);
}

static uint32_t slot_u32(unsigned off)
{
    return hdr_u32(off);
}

static void slot_wr_u32(unsigned off, uint32_t v)
{
    hdr_wr_u32(off, v);
}

/* ---- LOCK-FREE READING (§2 of the protocol doc, "read lock-free") ----
 * v2 does NOT take the app's semaphores at all. Hand-rolled semaphore
 * interop (futex wait/posts against the uClibc-layout words) wedged
 * the camera twice: our post ordering vs the app's wake-then-inc
 * corrupts the {0,1} invariant, the write lock ends held by nobody,
 * and rmm's whole pipeline stalls (the seq froze live, the user's
 * camera view with it). The doc's own lock-free analysis shows a
 * reader does not need the locks:
 *
 *   - the writer publishes seq/valid/head BEFORE copying the record
 *     bytes, one record at a time, in order - so a record with
 *     seq < hdr[0x18] is COMPLETE by construction (only the newest
 *     record can be mid-copy). Consuming strictly older records is
 *     tear-free.
 *   - the walk validates each record's magic and stride; anything
 *     torn or stale stops the walk (retried on the next poll).
 *   - our slot writes (cursor/pending/filter) race benignly: the
 *     writer never reads a cursor, and a lost pending clear only
 *     costs one extra scan.
 *
 * The result cannot wedge the app: we hold nothing, ever. */

/* ---- wrapping copy within the record ring ---- */
static void ring_copy(uint32_t off, void *dst, size_t n)
{
    size_t first;
    if (off >= DATA_SZ)
        off %= DATA_SZ;
    first = DATA_SZ - off;
    if (first > n)
        first = n;
    memcpy(dst, (const void *)(buf + DATA_OFF + off), first);
    if (first < n)
        memcpy((uint8_t *)dst + first, (const void *)(buf + DATA_OFF),
               n - first);
}

/* ---- the record header and the filter test (§3) ---- */
typedef struct {
    uint32_t len, seq, magic, cookie, ts;   /* +0..+16 */
    uint16_t type, chain, extra;            /* +20..+24 (26 bytes total) */
} rec_hdr;

/* Is this record wanted by a slot with this cursor and filter? The
 * writer only applies the class AND (that is what sets pending); the
 * subtype and the cursor freshness tests live here, in the reader. */
static int entry_wanted(uint32_t cursor, uint16_t filter, const rec_hdr *h)
{
    if ((filter & 0x0F) && (filter & 0x0F) != (h->type & 0x0F))
        return 0;
    if (!(h->type & (filter & 0xFF00)))
        return 0;
    if (cursor == 0)                     /* fresh reader: anything */
        return 1;
    {
        uint32_t want = (cursor == 0xFFFFFFFFu) ? 1u : cursor + 1;
        return (uint32_t)(h->seq - want) <= 0x7FFFFFFEu;   /* wrap-safe */
    }
}

/* ---- fifo purger thread (from the v1 walk, unchanged) ----
 * Holds the read end open (a fifo writer with no reader gets EPIPE)
 * and trims an idle-full fifo to a fresh ~224 KB tail so the next
 * client does not get served minutes-old content. Must never consume
 * the head while the fifo is NOT full. */
static void *fifo_purger_thread(void *arg)
{
    const char *fifo_name = arg;
    int fd = open(fifo_name, O_RDONLY);
    uint8_t drain[65536];

    if (fd < 0)
        return NULL;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    while (1) {
        int n = 0;
        if (ioctl(fd, FIONREAD, &n) == 0 && n > 224 * 1024) {
            while (n > 224 * 1024) {
                size_t want = (size_t)n - 224 * 1024;
                ssize_t r;
                if (want > sizeof(drain))
                    want = sizeof(drain);
                r = read(fd, drain, want);
                if (r <= 0)
                    break;
                n -= (int)r;
            }
        }
        usleep(250 * 1000);
    }
    return NULL;
}

/* ---- SPS/PPS parsing (from the v1 walk, unchanged) ---- */
static int sps_read_bit(const uint8_t *s, size_t slen, size_t *pos)
{
    int b;
    if (*pos >= slen * 8)
        return 0;
    b = (s[*pos >> 3] >> (7 - (*pos & 7))) & 1;
    (*pos)++;
    return b;
}

static unsigned sps_read_ue(const uint8_t *s, size_t slen, size_t *pos)
{
    unsigned z = 0, v = 0, i;
    while (*pos < slen * 8 && sps_read_bit(s, slen, pos) == 0)
        z++;
    for (i = 0; i < z && i < 24; i++)
        v = (v << 1) | (unsigned)sps_read_bit(s, slen, pos);
    return z > 24 ? 0xFFFFU : ((1u << z) - 1 + v);
}

static int sps_read_se(const uint8_t *s, size_t slen, size_t *pos)
{
    unsigned k = sps_read_ue(s, slen, pos);
    return (int)((k & 1) ? (k + 1) / 2 : -(int)(k / 2));
}

#define SPS_UE(label, var) \
    do { (var) = sps_read_ue(stripped, slen, &pos); \
         if (f2f_trace()) fprintf(stderr, "  %s = %u (pos %zu)\n", \
                                  label, (unsigned)(var), pos); } while (0)

static int parse_sps_dims(const uint8_t *d, size_t len, unsigned *w, unsigned *h)
{
    uint8_t stripped[NAL_BUF_MAX];
    size_t slen = 0;
    size_t i, pos;
    int profile;
    unsigned val;

    if (len < 4 || len > NAL_BUF_MAX)
        return 0;
    for (i = 0; i < len && slen < sizeof(stripped); i++) {
        if (slen >= 2 && stripped[slen - 1] == 0 && stripped[slen - 2] == 0 &&
            d[i] == 3)
            continue;
        stripped[slen++] = d[i];
    }
    if (slen < 4)
        return 0;

    profile = stripped[1];
    pos = 32;                        /* nal header + profile/constraints/level */
    SPS_UE("seq_parameter_set_id", val);
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        unsigned chroma, m;
        SPS_UE("chroma_format_idc", chroma);
        if (chroma == 3)
            (void)sps_read_bit(stripped, slen, &pos);
        SPS_UE("bit_depth_luma_minus8", val);
        SPS_UE("bit_depth_chroma_minus8", val);
        (void)sps_read_bit(stripped, slen, &pos);
        if (sps_read_bit(stripped, slen, &pos)) {
            for (m = 0; m < (chroma != 3 ? 8 : 12); m++) {
                if (sps_read_bit(stripped, slen, &pos)) {
                    unsigned n = (m < 6) ? 16 : 64;
                    unsigned last = 8, nxt = 8;
                    for (i = 0; i < n; i++) {
                        if (nxt != 0)
                            nxt = (last + sps_read_ue(stripped, slen, &pos)) % 256;
                        last = nxt != 0 ? nxt : last;
                    }
                }
            }
        }
    }
    SPS_UE("log2_max_frame_num_minus4", val);
    SPS_UE("pic_order_cnt_type", val);
    if (val == 0) {
        SPS_UE("log2_max_pic_order_cnt_lsb_minus4", val);
    } else if (val == 1) {
        (void)sps_read_bit(stripped, slen, &pos);
        SPS_UE("offset_for_non_ref_pic", val);
        SPS_UE("offset_for_top_to_bottom_field", val);
        SPS_UE("num_ref_frames_in_poc_cycle", val);
        for (i = 0; i < val && i < 255; i++)
            (void)sps_read_ue(stripped, slen, &pos);
    }
    SPS_UE("max_num_ref_frames", val);
    (void)sps_read_bit(stripped, slen, &pos);
    SPS_UE("pic_width_in_mbs_minus1", val);
    SPS_UE("pic_height_in_map_units_minus1", i);
    if (val == 0xFFFF || i == 0xFFFF || val > 300 || i > 300)
        return 0;
    *w = (val + 1) * 16;
    *h = (i + 1) * 16;
    if (sps_read_bit(stripped, slen, &pos) == 0)
        *h *= 2;
    return 1;
}

/* The SPS's true body length: like the PPS, the app glues metadata
 * after the rbsp_stop, and the decoder parses that junk as the VUI
 * ("Overread VUI by 8 bits", then garbage pps_ids). Walk the WHOLE
 * SPS grammar - dims, cropping, the VUI with its HRD sub-grammar -
 * then the stop bit is the first 1 followed by only zeros to the
 * byte boundary. Returns the body length, or 0 if the parse fails
 * (callers then keep the untrimmed emission). */
static size_t parse_sps_len(const uint8_t *d, size_t len)
{
    uint8_t stripped[NAL_BUF_MAX];
    size_t slen = 0;
    size_t i, pos;
    unsigned v, chroma = 1;
    int profile, fmbo;

    if (len < 4 || len > NAL_BUF_MAX)
        return 0;
    for (i = 0; i < len && slen < sizeof(stripped); i++) {
        if (slen >= 2 && stripped[slen - 1] == 0 && stripped[slen - 2] == 0 &&
            d[i] == 3)
            continue;
        stripped[slen++] = d[i];
    }
    if (slen < 4)
        return 0;

    profile = stripped[1];
    pos = 32;                        /* nal header + profile/constraints/level */
    SPS_UE("sps: seq_parameter_set_id", v);
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        SPS_UE("sps: chroma_format_idc", chroma);
        if (chroma == 3)
            (void)sps_read_bit(stripped, slen, &pos);
        SPS_UE("sps: bit_depth_luma_minus8", v);
        SPS_UE("sps: bit_depth_chroma_minus8", v);
        (void)sps_read_bit(stripped, slen, &pos); /* qpprime bypass */
        if (sps_read_bit(stripped, slen, &pos)) { /* scaling matrix present */
            unsigned m;
            for (m = 0; m < (chroma != 3 ? 8 : 12); m++) {
                if (sps_read_bit(stripped, slen, &pos)) { /* list present */
                    unsigned n = (m < 6) ? 16 : 64;
                    unsigned last = 8, nxt = 8;
                    for (i = 0; i < n; i++) {
                        if (nxt != 0) {
                            int ds = sps_read_se(stripped, slen, &pos);
                            nxt = (unsigned)((int)last + ds + 256) % 256;
                        }
                        if (nxt != 0)
                            last = nxt;
                    }
                }
            }
        }
    }
    SPS_UE("sps: log2_max_frame_num_minus4", v);
    SPS_UE("sps: pic_order_cnt_type", v);
    if (v == 0) {
        SPS_UE("sps: log2_max_pic_order_cnt_lsb_minus4", v);
    } else if (v == 1) {
        (void)sps_read_bit(stripped, slen, &pos);
        (void)sps_read_se(stripped, slen, &pos);
        (void)sps_read_se(stripped, slen, &pos);
        SPS_UE("sps: num_ref_frames_in_poc_cycle", v);
        for (i = 0; i < v && i < 255; i++)
            (void)sps_read_se(stripped, slen, &pos);
    }
    SPS_UE("sps: max_num_ref_frames", v);
    (void)sps_read_bit(stripped, slen, &pos); /* gaps flag */
    SPS_UE("sps: pic_width_in_mbs_minus1", v);
    SPS_UE("sps: pic_height_in_map_units_minus1", v);
    fmbo = sps_read_bit(stripped, slen, &pos); /* frame_mbs_only */
    if (!fmbo)
        (void)sps_read_bit(stripped, slen, &pos); /* mb_adaptive */
    (void)sps_read_bit(stripped, slen, &pos); /* direct_8x8 */
    if (sps_read_bit(stripped, slen, &pos)) {  /* frame_cropping_flag */
        (void)sps_read_ue(stripped, slen, &pos);
        (void)sps_read_ue(stripped, slen, &pos);
        (void)sps_read_ue(stripped, slen, &pos);
        (void)sps_read_ue(stripped, slen, &pos);
    }
    if (sps_read_bit(stripped, slen, &pos)) {  /* vui_parameters_present */
        unsigned hrd = 0;
        if (sps_read_bit(stripped, slen, &pos)) { /* aspect_ratio_info */
            v = 0;
            {
                unsigned k;
                for (k = 0; k < 8; k++)
                    v = (v << 1) | (unsigned)sps_read_bit(stripped, slen, &pos);
            }
            if (v == 255) {
                unsigned k;
                for (k = 0; k < 16; k++)
                    (void)sps_read_bit(stripped, slen, &pos);
                for (k = 0; k < 16; k++)
                    (void)sps_read_bit(stripped, slen, &pos);
            }
        }
        if (sps_read_bit(stripped, slen, &pos)) /* overscan_info */
            (void)sps_read_bit(stripped, slen, &pos);
        if (sps_read_bit(stripped, slen, &pos)) { /* video_signal_type */
            (void)sps_read_bit(stripped, slen, &pos);
            (void)sps_read_bit(stripped, slen, &pos);
            (void)sps_read_bit(stripped, slen, &pos);
            if (sps_read_bit(stripped, slen, &pos)) { /* colour_description */
                unsigned k;
                for (k = 0; k < 24; k++)
                    (void)sps_read_bit(stripped, slen, &pos);
            }
        }
        if (sps_read_bit(stripped, slen, &pos)) { /* chroma_loc_info */
            (void)sps_read_ue(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
        }
        if (sps_read_bit(stripped, slen, &pos)) { /* timing_info */
            unsigned k;
            for (k = 0; k < 32; k++)
                (void)sps_read_bit(stripped, slen, &pos);
            for (k = 0; k < 32; k++)
                (void)sps_read_bit(stripped, slen, &pos);
            (void)sps_read_bit(stripped, slen, &pos);
        }
        if (sps_read_bit(stripped, slen, &pos)) { /* nal_hrd_parameters */
            unsigned cpb, s;
            SPS_UE("sps: hrd cpb_cnt_minus1", cpb);
            for (i = 0; i < 4; i++)
                (void)sps_read_bit(stripped, slen, &pos);
            for (i = 0; i < 4; i++)
                (void)sps_read_bit(stripped, slen, &pos);
            for (s = 0; s <= cpb && s < 32; s++) {
                (void)sps_read_ue(stripped, slen, &pos);
                (void)sps_read_ue(stripped, slen, &pos);
                (void)sps_read_bit(stripped, slen, &pos);
            }
            for (i = 0; i < 20; i++)
                (void)sps_read_bit(stripped, slen, &pos);
            hrd = 1;
        }
        if (sps_read_bit(stripped, slen, &pos)) { /* vcl_hrd_parameters */
            unsigned cpb, s;
            SPS_UE("sps: vcl hrd cpb_cnt_minus1", cpb);
            for (i = 0; i < 4; i++)
                (void)sps_read_bit(stripped, slen, &pos);
            for (i = 0; i < 4; i++)
                (void)sps_read_bit(stripped, slen, &pos);
            for (s = 0; s <= cpb && s < 32; s++) {
                (void)sps_read_ue(stripped, slen, &pos);
                (void)sps_read_ue(stripped, slen, &pos);
                (void)sps_read_bit(stripped, slen, &pos);
            }
            for (i = 0; i < 20; i++)
                (void)sps_read_bit(stripped, slen, &pos);
            hrd = 1;
        }
        if (hrd)
            (void)sps_read_bit(stripped, slen, &pos); /* low_delay_hrd */
        (void)sps_read_bit(stripped, slen, &pos); /* pic_struct_present */
        if (sps_read_bit(stripped, slen, &pos)) {  /* bitstream_restriction */
            (void)sps_read_bit(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
            (void)sps_read_ue(stripped, slen, &pos);
        }
    }
    /* the rbsp stop: the next 1 bit, followed by only zeros to the
     * byte boundary (bytes beyond are the glued metadata) */
    {
        size_t probe = pos;
        int one = -1;
        while (probe < slen * 8) {
            if (sps_read_bit(stripped, slen, &probe) == 1) {
                one = (int)probe - 1;
                break;
            }
        }
        if (one >= 0) {
            size_t after = (size_t)one + 1;
            size_t byte_end = ((size_t)one / 8 + 1) * 8;
            int all_zero = 1;
            while (after < byte_end && after < slen * 8) {
                if (sps_read_bit(stripped, slen, &after) != 0) {
                    all_zero = 0;
                    break;
                }
            }
            if (!all_zero)
                return 0;              /* a real extension follows */
            return ((size_t)one + 7) / 8;
        }
    }
    return 0;
}

/* The PPS's true body length: the app may glue metadata after the
 * rbsp_stop; the decoder parses that junk as PPS syntax and dies. The
 * grammar is short; returns the body length or 0 to keep the whole. */
static size_t parse_pps_len(const uint8_t *d, size_t len)
{
    uint8_t stripped[NAL_BUF_MAX];
    size_t slen = 0;
    size_t i, pos;
    unsigned v;

    if (len < 2 || len > NAL_BUF_MAX)
        return 0;
    for (i = 0; i < len && slen < sizeof(stripped); i++) {
        if (slen >= 2 && stripped[slen - 1] == 0 && stripped[slen - 2] == 0 &&
            d[i] == 3)
            continue;
        stripped[slen++] = d[i];
    }
    if (slen < 2)
        return 0;

    pos = 8;
    SPS_UE("pps: pic_parameter_set_id", v);
    SPS_UE("pps: seq_parameter_set_id", v);
    (void)sps_read_bit(stripped, slen, &pos);
    (void)sps_read_bit(stripped, slen, &pos);
    SPS_UE("pps: num_slice_groups_minus1", v);
    if (v > 0)
        return 0;
    SPS_UE("pps: num_ref_idx_l0_default_active_minus1", v);
    SPS_UE("pps: num_ref_idx_l1_default_active_minus1", v);
    (void)sps_read_bit(stripped, slen, &pos);
    (void)sps_read_bit(stripped, slen, &pos);
    (void)sps_read_bit(stripped, slen, &pos);
    (void)sps_read_se(stripped, slen, &pos);
    (void)sps_read_se(stripped, slen, &pos);
    (void)sps_read_se(stripped, slen, &pos);
    (void)sps_read_bit(stripped, slen, &pos);
    (void)sps_read_bit(stripped, slen, &pos);
    (void)sps_read_bit(stripped, slen, &pos);
    {
        size_t probe = pos;
        int one = -1;
        while (probe < slen * 8) {
            if (sps_read_bit(stripped, slen, &probe) == 1) {
                one = (int)probe - 1;
                break;
            }
        }
        if (one >= 0) {
            size_t after = (size_t)one + 1;
            size_t byte_end = ((size_t)one / 8 + 1) * 8;
            int all_zero = 1;
            while (after < byte_end && after < slen * 8) {
                if (sps_read_bit(stripped, slen, &after) != 0) {
                    all_zero = 0;
                    break;
                }
            }
            if (!all_zero)
                return 0;              /* a real extension follows */
            return ((size_t)one + 7) / 8;   /* stop bit position -> bytes */
        }
    }
    return 0;
}

/* ---- fifo writes ---- */
/* whole-or-drop: a P slice never blocks the reader; on a full fifo it
 * is dropped (the purger keeps the tail fresh for the next join). */
static int write_whole_fd(int fd, const void *p, size_t n)
{
    ssize_t w = write(fd, p, n);
    if (w < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    if ((size_t)w != n)
        return -1;
    return 0;
}

/* wait-for-space: chain NALs (SPS/PPS/IDR) must arrive whole - a
 * dropped SPS breaks every client join. */
static int write_all_fd_wait(int fd, const void *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const uint8_t *)p + off, n - off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10 * 1000);
                continue;
            }
            return -2;
        }
        off += (size_t)w;
    }
    return 0;
}

/* ---- chain state (join quality) ----
 * stage: 0 = no accepted SPS yet, 1 = SPS seen, 2 = SPS+PPS seen. The
 * IDR completes the chain and opens the stream; P slices are emitted
 * only once a chain has completed, so every client's first frames
 * decode cleanly instead of mid-GOP. */
static int stage;
static int chain_open;
static unsigned target_w, target_h;
static int one_shot;                  /* -n: dump 256 records and exit */
static int one_shot_done;             /* -n budget exhausted */

/* Emit one delivered record: the payload is ONE OR MORE NALs, each
 * with a 4-byte start code (a 0/5/6-byte extras prefix may precede
 * the first). The 0x0401 record carries the era's whole GOP head -
 * SPS + PPS + ~20 zeros + the IDR - in one payload, so every NAL
 * inside must be walked and routed through the chain gate by its NAL
 * type (the record type itself is not the content type).
 * Returns 0, -1 (dropped), or -2 (hard error). */
static int emit_record(const uint8_t *p, uint32_t len, uint16_t type)
{
    uint32_t i = 0;
    int r = 0;
    int nsc = 0;

    while (i + 4 <= len) {
        uint32_t s, e;
        uint8_t nt;
        int chain_nal;

        /* next start code */
        while (i + 4 <= len &&
               !(p[i] == 0x00 && p[i + 1] == 0x00 &&
                 p[i + 2] == 0x00 && p[i + 3] == 0x01))
            i++;
        if (i + 4 > len)
            break;
        s = i;
        e = s + 4;
        /* the NAL ends at the next start code */
        while (e + 4 <= len &&
               !(p[e] == 0x00 && p[e + 1] == 0x00 &&
                 p[e + 2] == 0x00 && p[e + 3] == 0x01))
            e++;
        if (e - s < 5) {               /* empty NAL: skip */
            i = e;
            continue;
        }
        nt = p[s + 4] & 0x1F;
        nsc++;

        /* chain gate by NAL type */
        if (nt == 7) {                 /* SPS: opens the stage */
            unsigned w = 0, h = 0;
            size_t tl = parse_sps_len(p + s + 4, (size_t)(e - s - 4));
            if (parse_sps_dims(p + s + 4, e - s - 4, &w, &h)) {
                if ((uint64_t)w * h > (uint64_t)target_w * target_h) {
                    target_w = w;
                    target_h = h;
                }
            }
            if (tl > 0 && tl + 4 < (size_t)(e - s))
                e = s + 4 + (uint32_t)tl;   /* trim the glued metadata */
            stage = 1;
            chain_nal = 1;
        } else if (nt == 8) {          /* PPS */
            if (stage < 1) {
                i = e;
                continue;              /* before any SPS: drop */
            }
            {
                /* The PPS NAL runs to the next start code - the
                 * zero padding before the IDR rides inside it, and
                 * the decoder parses that as PPS syntax and dies
                 * ("pps_id out of range"). Trim to the rbsp stop. */
                size_t tl = parse_pps_len(p + s + 4, (size_t)(e - s - 4));
                if (tl > 0 && tl + 4 < (size_t)(e - s))
                    e = s + 4 + (uint32_t)tl;
            }
            stage = 2;
            chain_nal = 1;
        } else if (nt == 5) {          /* IDR: completes the chain */
            if (stage < 2) {
                i = e;
                continue;
            }
            chain_open = 1;
            stage = 0;
            chain_nal = 1;
        } else if (nt == 1) {          /* P slice */
            if (!chain_open) {
                i = e;
                continue;              /* mid-GOP: drop until the join */
            }
            chain_nal = 0;             /* whole-or-drop */
        } else {                       /* SEI/AUD/junk */
            if (!chain_open && stage == 0) {
                i = e;
                continue;
            }
            chain_nal = 1;
        }
        if (f2f_trace())
            fprintf(stderr,
                    "rec: type=0x%04x nal%d nt=%d len=%u nlen=%u "
                    "stage=%d open=%d\n",
                    type, nsc, nt, len, e - s, stage, chain_open);

        /* chain NALs (SPS/PPS/IDR) must arrive whole; P slices are
         * whole-or-drop so a stalled client never blocks the reader */
        if (out_fd >= 0) {
            r = chain_nal ? write_all_fd_wait(out_fd, p + s, e - s)
                          : write_whole_fd(out_fd, p + s, e - s);
        } else {
            r = (fwrite(p + s, 1, e - s, out) == (size_t)(e - s)) ? 0 : -2;
            if (out)
                fflush(out);
        }
        if (r == 0) {
            g_nals++;
            g_bytes += e - s;
        } else if (r == -1) {
            g_drops++;
        } else if (r == -2) {
            return r;
        }
        i = e;
    }
    return r;
}

/* ---- the reader loop (§5.4, lock-free) ----
 * Poll slot[K].pending on a short timer (the notify semaphore is
 * deliberately skipped); while pending, walk the ring from the
 * published tail. No locks are taken: records with seq strictly
 * below the published newest seq are complete by construction (the
 * writer publishes seq/valid/head BEFORE the record bytes, one
 * record at a time), and the magic/stride validation stops the walk
 * on anything torn. The slot writes race benignly. */
static void reader_loop(void)
{
    static uint8_t *payload;
    static uint32_t cur_len;
    static uint16_t cur_type;
    time_t next_log = 0;
    unsigned budget = 256;             /* -n one-shot budget */

    payload = malloc(MAXPAY);
    if (!payload) {
        perror("malloc");
        return;
    }

    while (!stop_flag) {
        int got = 0;

        if (slot_u32(SLOT_OFF(slot_k)) != 0) {         /* pending */
            uint32_t off = hdr_u32(HDR_TAIL);
            uint32_t left = hdr_u32(HDR_VALID);
            uint32_t newest = hdr_u32(HDR_SEQ);

            while (left > 26) {
                rec_hdr h;
                uint32_t mh;
                ring_copy(off, &h, sizeof(h));
                mh = h.magic & 0xffff0000u;
                if (mh != 0x6a8c0000u && mh != 0x6a890000u)
                    break;             /* torn/stale: stop, retry next poll */
                if (26 + h.len > left || h.len > MAXPAY)
                    break;
                if (h.seq == newest)
                    break;             /* the newest record may be mid-copy */
                if (entry_wanted(slot_cursor, FILTER_HIRES, &h)) {
                    if (h.len <= MAXPAY) {
                        ring_copy((uint32_t)(off + 26), payload, h.len);
                        cur_len = h.len;
                        cur_type = h.type;
                        got = 1;
                    }
                    slot_cursor = h.seq;
                    slot_wr_u32(SLOT_OFF(slot_k) + 8, h.seq);
                    if (h.seq + 1 == newest)
                        slot_wr_u32(SLOT_OFF(slot_k), 0);   /* caught up */
                    break;
                }
                off = (off + 26 + h.len) % DATA_SZ;
                left -= 26 + h.len;
            }
            if (!got)
                slot_wr_u32(SLOT_OFF(slot_k), 0);  /* nothing for us now */
        }

        if (f2f_agelog()) {
            time_t t = time(NULL);
            if (t >= next_log) {
                fprintf(stderr,
                        "age: nals=%u bytes=%u drops=%u cursor=%u seq=%u "
                        "valid=%u\n",
                        g_nals, g_bytes, g_drops, slot_cursor,
                        hdr_u32(HDR_SEQ), hdr_u32(HDR_VALID));
                g_nals = g_bytes = g_drops = 0;
                next_log = t + 1;
            }
        }

        if (!got) {
            usleep(POLL_MS * 1000);
            continue;
        }

        if (one_shot && budget && --budget == 0)
            one_shot_done = 1;
        if (emit_record(payload, cur_len, cur_type) == -2) {
            perror("write");
            return;
        }
        if (one_shot_done)
            return;
    }
}

/* ---- slot claim + the join scan (lock-free) ----
 * Slots 10..14 are ours (the stock consumers use 0, 1, 2, 16). Claim
 * the first with a zero filter - or a 0x0400 filter left by our own
 * previous run (stale cursor, harmless to re-seed). Seed the cursor
 * at the newest 0x0422 chain start so the first emission is a
 * complete SPS->PPS->IDR chain. The slot writes are plain aligned
 * stores; the writer reads the filter under its own lock and never
 * reads a cursor, so there is nothing to race on. */
static int claim_slot(int want_slot)
{
    int k;
    int first = want_slot >= 0 ? want_slot : 10;
    int last = want_slot >= 0 ? want_slot : 14;

    for (k = first; k <= last; k++) {
        uint32_t filt;
        uint32_t best = 0;

        filt = hdr_u32(SLOT_OFF(k) + 12) & 0xFFFF;
        if (filt != 0 && filt != FILTER_HIRES)
            continue;                  /* occupied by someone else */
        /* seed the cursor: the newest 0x0422 with at least 4 records
         * after it (its IDR is then written too: a complete chain) */
        {
            uint32_t off = hdr_u32(HDR_TAIL);
            uint32_t left = hdr_u32(HDR_VALID);
            uint32_t newest = hdr_u32(HDR_SEQ);
            while (left > 26) {
                rec_hdr h;
                uint32_t mh;
                ring_copy(off, &h, sizeof(h));
                mh = h.magic & 0xffff0000u;
                if (mh != 0x6a8c0000u && mh != 0x6a890000u)
                    break;
                if (26 + h.len > left || h.len > MAXPAY)
                    break;
                if (h.seq == newest)
                    break;
                if (h.type == 0x0422 && h.seq < newest - 4)
                    best = h.seq;
                off = (off + 26 + h.len) % DATA_SZ;
                left -= 26 + h.len;
            }
        }
        /* cursor = the chain start MINUS one: the 0x0422 itself must
         * be delivered (entry_wanted wants seq >= cursor+1) so the
         * chain gate can open on it */
        if (best)
            best--;
        slot_wr_u32(SLOT_OFF(k) + 8, best ? best : hdr_u32(HDR_SEQ));
        slot_wr_u32(SLOT_OFF(k), 1);          /* pending: poll starts */
        slot_cursor = best ? best : hdr_u32(HDR_SEQ);
        /* publish the filter: a single aligned u16 store */
        {
            uint16_t f = FILTER_HIRES;
            memcpy((void *)(buf + SLOT_OFF(k) + 12), &f, 2);
        }
        slot_k = k;
        fprintf(stderr, "slot %d claimed (filter 0x%04x, cursor %u)\n",
                k, FILTER_HIRES, slot_cursor);
        return 0;
    }
    fprintf(stderr, "no free slot in %d..%d\n", first, last);
    return 1;
}

/* release our slot (best effort on the way out) */
static void release_slot(void)
{
    uint16_t zero = 0;
    if (slot_k < 0)
        return;
    memcpy((void *)(buf + SLOT_OFF(slot_k) + 12), &zero, 2);
    slot_wr_u32(SLOT_OFF(slot_k), 0);
    slot_wr_u32(SLOT_OFF(slot_k) + 4, 0);
    slot_wr_u32(SLOT_OFF(slot_k) + 8, 0);
    fprintf(stderr, "slot %d released\n", slot_k);
    slot_k = -1;
}

static void on_term(int sig)
{
    (void)sig;
    stop_flag = 1;
}

int main(int argc, char **argv)
{
    const char *fifo_name = DEFAULT_FIFO;
    const char *out_file = NULL;
    int want_slot = -1;
    int opt;
    int fd;
    struct stat st;
    pthread_t unlock_thread;

    while ((opt = getopt(argc, argv, "f:o:ns:h")) != -1) {
        switch (opt) {
        case 'f': fifo_name = optarg; break;
        case 'o': out_file = optarg; break;
        case 'n': one_shot = 1; break;
        case 's': want_slot = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-f FIFO] [-o FILE] [-n] [-s SLOT]\n",
                    argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    fd = open(BUFFER_FILE, O_RDWR);
    if (fd < 0) {
        perror(BUFFER_FILE);
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    buf_size = (size_t)st.st_size;
    buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    if (buf_size < DATA_OFF + DATA_SZ) {
        fprintf(stderr, "%s too small: %zu\n", BUFFER_FILE, buf_size);
        return 1;
    }

    /* The locks are deliberately NOT opened: v2 reads lock-free (see
     * the LOCK-FREE READING note). The futex interop wedged the
     * camera twice; holding nothing cannot wedge anything. */

    /* the fifo is opened before the slot claim so a server can attach
     * immediately and just see no bytes until the first chain */
    if (out_file) {
        out = fopen(out_file, "wb");
        if (!out) {
            perror(out_file);
            return 1;
        }
    } else {
        if (mkfifo(fifo_name, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo");
            return 1;
        }
        pthread_create(&unlock_thread, NULL, fifo_purger_thread,
                       (void *)fifo_name);
        {
            int pf = open(fifo_name, O_RDONLY | O_NONBLOCK);
            if (pf >= 0) {
                int ps = fcntl(pf, F_SETPIPE_SZ, 1024 * 1024);
                fprintf(stderr, "pipe probe: set 1MB -> %d (%s)\n",
                        ps, ps != 0 ? strerror(errno) : "ok");
                out_fd = open(fifo_name, O_WRONLY);
                close(pf);
                if (out_fd < 0) {
                    perror(fifo_name);
                    return 1;
                }
            } else {
                fprintf(stderr, "pipe probe: open failed (%s)\n",
                        strerror(errno));
                out_fd = open(fifo_name, O_WRONLY);
                if (out_fd < 0) {
                    perror(fifo_name);
                    return 1;
                }
            }
        }
        fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL, 0) | O_NONBLOCK);
        {
            int cur = fcntl(out_fd, F_GETPIPE_SZ);
            if (cur > 0)
                g_pipe_size = (size_t)cur;
        }
    }

    if (claim_slot(want_slot) != 0)
        return 1;

    reader_loop();

    release_slot();
    if (out)
        fclose(out);
    if (out_fd >= 0)
        close(out_fd);
    munmap((void *)buf, buf_size);
    return 0;
}
