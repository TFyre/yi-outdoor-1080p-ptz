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
 * Audio mode (-a, or run as f2f_audio): filter 0x0100 instead and
 * pass each record's payload straight through to
 * /tmp/aac_audio_fifo. The payload IS one complete ADTS frame - the
 * app writes MPEG-2 ADTS directly (verified on the ring snapshots:
 * ff f9 sync, profile AAC-LC, 16 kHz, mono, and the ADTS
 * frame_length field equals the record length exactly), so no
 * re-framing is needed. The server's ADTSAudioFifoSource parses the
 * ADTS header for the SDP config and strips it per frame. There is no
 * audio chain (no 0x01xx records exist); the ADTS header is the whole
 * codec config.
 *
 * Usage: fshare2fifo [-a] [-f FIFO] [-o FILE] [-n] [-s SLOT]
 *   default output: fifo /tmp/h264_high_fifo (audio: /tmp/aac_audio_fifo)
 *   -a: audio mode (filter 0x0100, ADTS passthrough)
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
#define FILTER_AUDIO 0x0100u
#define POLL_MS 10
#define MAXPAY  (512u * 1024)    /* the 0x0401 record carries the whole
                                    GOP head (SPS+PPS+zeros+IDR): 78 KB
                                    calm, 300-500 KB at IR-noise rates */
#define NAL_BUF_MAX 256          /* SPS/PPS are tiny */
#define DEFAULT_FIFO "/tmp/h264_high_fifo"
#define DEFAULT_AUDIO_FIFO "/tmp/aac_audio_fifo"
/* purger keep-threshold: ~5 s of 1080p at a calm bitrate for video,
 * ~6 s of audio (171 B frames at 15.6/s) for audio */
#define PURGE_TRIM_VIDEO (224u * 1024)
#define PURGE_TRIM_AUDIO (16u * 1024)

/* Stall watchdog (see reader_loop): ring flowing (valid moves) but no
 * record consumed for STALL_SECS -> re-anchor the cursor at the newest
 * seq; after MAX_STALL_REANCHOR consecutive re-anchors with nothing
 * consumed, exit and let start-rtsp.sh's watcher respawn us with a
 * fresh claim. */
#define STALL_SECS 15
#define MAX_STALL_REANCHOR 4

static volatile uint8_t *buf;      /* mmap of the shared buffer */
static size_t buf_size;
static int slot_k = -1;            /* our reader slot */
static const char *g_fifo_name = DEFAULT_FIFO;
static uint32_t slot_cursor;       /* mirrored slot[K].cursor */
static FILE *out;                  /* file output (-o FILE) */
static int out_fd = -1;            /* fifo output, non-blocking */
static size_t g_pipe_size = 256 * 1024;
static volatile int stop_flag;
static int audio_mode;             /* -a / argv[0]: ADTS passthrough */
static uint16_t g_filter = FILTER_HIRES;
static size_t g_purge_trim = PURGE_TRIM_VIDEO;
static void release_slot(void);   /* defined below reader_loop; the
                                   * stall watchdog calls it */

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

/* The record magic's high 16 bits are a family, not a constant: the
 * observed prefixes are 0x6a8c, 0x6a89, 0x6a8a (boot era) and 0x6a8f
 * (post-reboot) - only 0x6a8x is fixed. */
static int magic_ok(uint32_t m)
{
    /* The record magic's hi16 drifts per writer era: 0x6a8a, 0x6a8c were
     * observed, and on 2026-08-27 the app's writer moved to 0x6a90
     * mid-uptime - the old 0x6a80..0x6a8f range rejected every record
     * of the new era and the walk stalled forever (0 emission while
     * the seq counter kept marching). The prefix is not a class; the
     * low bytes are writer/sequence noise, and the real torn-record
     * protection is the cascade that follows this gate (len vs left and
     * MAXPAY, seq vs newest, type vs filter, next-record magic). Accept
     * any 0x6aXX so an era change can never stall the walk again. */
    return (m >> 16) >= 0x6a00 && (m >> 16) <= 0x6aff;
}

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
 * and trims an idle-full fifo to a fresh tail (g_purge_trim bytes) so
 * the next client does not get served minutes-old content. Must never
 * consume the head while the fifo is NOT full. */
static void *fifo_purger_thread(void *arg)
{
    const char *fifo_name = arg;
    int fd = open(fifo_name, O_RDONLY);
    uint8_t drain[65536];

    if (fd < 0) {
        fprintf(stderr, "purger: open %s failed (%s)\n",
                fifo_name, strerror(errno));
        return NULL;
    }
    fprintf(stderr, "purger: holding %s read end\n", fifo_name);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    while (1) {
        int n = 0;
        if (ioctl(fd, FIONREAD, &n) == 0 && n > (int)g_purge_trim) {
            while (n > (int)g_purge_trim) {
                size_t want = (size_t)n - g_purge_trim;
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

/* ---- SPS synthesis (replaces the stop-bit trim) ----
 * The era's SPS NAL carries the app's glued metadata after the rbsp
 * stop; the stop-bit heuristics proved unreliable (they landed inside
 * the junk and the decoder parsed it as a VUI: "Overread VUI by 8
 * bits"). Instead, REBUILD a canonical SPS from the parsed fields:
 * everything the slices depend on (log2_max_frame_num, pic_order_cnt,
 * max_num_ref_frames, the dims, frame_mbs_only) is taken from the
 * real SPS; the optional parts (cropping, VUI) are omitted. */
struct sps_info {
    unsigned profile, constraint, level, sps_id;
    unsigned log2frm, poc_type, poc_lsb, max_ref;
    unsigned w, h;
    int fmbo, mb_adaptive, direct8x8, crop_flag;
    unsigned crop[4];
    int ok;
};

static void parse_sps_full(const uint8_t *d, size_t len, struct sps_info *si)
{
    uint8_t stripped[NAL_BUF_MAX];
    size_t slen = 0;
    size_t i, pos;
    int profile;
    unsigned val;

    memset(si, 0, sizeof(*si));
    if (len < 4 || len > NAL_BUF_MAX)
        return;
    for (i = 0; i < len && slen < sizeof(stripped); i++) {
        if (slen >= 2 && stripped[slen - 1] == 0 && stripped[slen - 2] == 0 &&
            d[i] == 3)
            continue;
        stripped[slen++] = d[i];
    }
    if (slen < 4)
        return;

    profile = stripped[1];
    pos = 32;                        /* nal header + profile/constraints/level */
    si->profile = (unsigned)stripped[0 + 0];   /* the NAL header byte */
    si->profile = (unsigned)profile;
    si->constraint = (unsigned)stripped[2];
    si->level = (unsigned)stripped[3];
    SPS_UE("seq_parameter_set_id", val);
    si->sps_id = val;
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
    si->log2frm = val;
    SPS_UE("pic_order_cnt_type", val);
    si->poc_type = val;
    if (val == 0) {
        SPS_UE("log2_max_pic_order_cnt_lsb_minus4", val);
        si->poc_lsb = val;
    } else if (val == 1) {
        (void)sps_read_bit(stripped, slen, &pos);
        SPS_UE("offset_for_non_ref_pic", val);
        SPS_UE("offset_for_top_to_bottom_field", val);
        SPS_UE("num_ref_frames_in_poc_cycle", val);
        for (i = 0; i < val && i < 255; i++)
            (void)sps_read_ue(stripped, slen, &pos);
        si->poc_type = 9;              /* unsupported: no synthesis */
    }
    SPS_UE("max_num_ref_frames", val);
    si->max_ref = val;
    (void)sps_read_bit(stripped, slen, &pos);   /* gaps_in_frame_num */
    SPS_UE("pic_width_in_mbs_minus1", val);
    SPS_UE("pic_height_in_map_units_minus1", i);
    if (val == 0xFFFF || i == 0xFFFF || val > 300 || i > 300)
        return;
    si->w = (val + 1) * 16;
    si->h = (i + 1) * 16;
    si->fmbo = sps_read_bit(stripped, slen, &pos);
    if (!si->fmbo) {
        si->mb_adaptive = sps_read_bit(stripped, slen, &pos);
        si->h *= 2;
    }
    si->direct8x8 = sps_read_bit(stripped, slen, &pos);
    si->crop_flag = sps_read_bit(stripped, slen, &pos);
    if (si->crop_flag) {
        si->crop[0] = sps_read_ue(stripped, slen, &pos);
        si->crop[1] = sps_read_ue(stripped, slen, &pos);
        si->crop[2] = sps_read_ue(stripped, slen, &pos);
        si->crop[3] = sps_read_ue(stripped, slen, &pos);
        if (si->crop[0] == 0xFFFF || si->crop[1] == 0xFFFF ||
            si->crop[2] == 0xFFFF || si->crop[3] == 0xFFFF)
            return;
    }
    /* the VUI flag and its contents are intentionally NOT parsed: the
     * synthesis drops the VUI entirely */
    si->ok = 1;
}

static void sps_wbits(uint8_t *out, size_t *pos, uint32_t v, unsigned n)
{
    while (n--) {
        if (v & (1u << n))
            out[*pos / 8] |= (uint8_t)(1u << (7 - (*pos % 8)));
        (*pos)++;
    }
}

static void sps_wue(uint8_t *out, size_t *pos, uint32_t v)
{
    unsigned n = 0;
    uint32_t t = v + 1;
    while (t > 1) {
        t >>= 1;
        n++;
    }
    sps_wbits(out, pos, 0, n);        /* n leading zeros */
    sps_wbits(out, pos, v + 1, n + 1);
}

/* Build the canonical SPS NAL (header byte included). Returns the
 * byte length. */
static size_t build_sps(uint8_t *out, const struct sps_info *si)
{
    size_t pos = 0;

    memset(out, 0, NAL_BUF_MAX);
    out[0] = 0x67;
    pos = 8;
    sps_wbits(out, &pos, si->profile, 8);
    sps_wbits(out, &pos, si->constraint, 8);
    sps_wbits(out, &pos, si->level, 8);
    sps_wue(out, &pos, si->sps_id);
    sps_wue(out, &pos, si->log2frm);
    sps_wue(out, &pos, si->poc_type);
    if (si->poc_type == 0)
        sps_wue(out, &pos, si->poc_lsb);
    sps_wue(out, &pos, si->max_ref);
    sps_wbits(out, &pos, 0, 1);        /* gaps_in_frame_num */
    sps_wue(out, &pos, si->w / 16 - 1);
    sps_wue(out, &pos, (si->fmbo ? si->h : si->h / 2) / 16 - 1);
    sps_wbits(out, &pos, (unsigned)si->fmbo, 1);
    if (!si->fmbo)
        sps_wbits(out, &pos, (unsigned)si->mb_adaptive, 1);
    sps_wbits(out, &pos, (unsigned)si->direct8x8, 1);
    sps_wbits(out, &pos, (unsigned)si->crop_flag, 1);
    if (si->crop_flag) {
        sps_wue(out, &pos, si->crop[0]);
        sps_wue(out, &pos, si->crop[1]);
        sps_wue(out, &pos, si->crop[2]);
        sps_wue(out, &pos, si->crop[3]);
    }
    sps_wbits(out, &pos, 0, 1);        /* no VUI */
    sps_wbits(out, &pos, 1, 1);        /* rbsp stop */
    while (pos % 8)
        sps_wbits(out, &pos, 0, 1);    /* trailing zeros */
    return pos / 8;
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
        /* the NAL ends at the next start code, else at the payload
         * end (the old scan stopped at len-3, cutting the last three
         * bytes of every record's final NAL - the decoder then
         * overread each slice's cabac tail by 5-9 bytes and
         * concealed the bottom macroblock row) */
        while (e + 4 <= len &&
               !(p[e] == 0x00 && p[e + 1] == 0x00 &&
                 p[e + 2] == 0x00 && p[e + 3] == 0x01))
            e++;
        if (e + 4 > len)
            e = len;
        if (e - s < 5) {               /* empty NAL: skip */
            i = e;
            continue;
        }
        nt = p[s + 4] & 0x1F;
        nsc++;

        /* chain gate by NAL type */
        if (nt == 7) {                 /* SPS: opens the stage */
            struct sps_info si;
            uint8_t spsbuf[NAL_BUF_MAX];
            size_t sl;

            parse_sps_full(p + s + 4, (size_t)(e - s - 4), &si);
            if (si.ok && si.poc_type == 0) {
                sl = build_sps(spsbuf, &si);
                if ((uint64_t)si.w * si.h >
                    (uint64_t)target_w * target_h) {
                    target_w = si.w;
                    target_h = si.h;
                }
                stage = 1;
                if (f2f_trace())
                    fprintf(stderr,
                            "rec: type=0x%04x nt=%d synth=%zu (%ux%u "
                            "fmbo=%d mb=%d d8=%d crop=%d [%u %u %u %u])\n",
                            type, nt, sl, si.w, si.h, si.fmbo,
                            si.mb_adaptive, si.direct8x8, si.crop_flag,
                            si.crop[0], si.crop[1], si.crop[2], si.crop[3]);
                /* emit SC + the synthesized SPS (chain write) */
                if (out_fd >= 0) {
                    r = write_all_fd_wait(out_fd, p + s, 4);
                    if (r == 0)
                        r = write_all_fd_wait(out_fd, spsbuf, sl);
                } else {
                    r = (fwrite(p + s, 1, 4, out) == 4 &&
                         fwrite(spsbuf, 1, sl, out) == sl) ? 0 : -2;
                    if (out)
                        fflush(out);
                }
                if (r == 0) {
                    g_nals++;
                    g_bytes += 4 + sl;
                }
                i = e;
                continue;
            }
            /* parse failed or an unsupported poc type: emit the
             * original NAL untrimmed */
            stage = 1;
            chain_nal = 1;
        } else if (nt == 8) {          /* PPS */
            uint32_t body = e - s - 4;
            if (stage < 1) {
                i = e;
                continue;              /* before any SPS: drop */
            }
            /* The PPS NAL runs to the next start code: the zero
             * padding before the IDR rides inside it and the decoder
             * parses that as PPS syntax and dies ("pps_id out of
             * range"). The body ends at the last nonzero byte (the
             * stop/padding byte). Some chains carry a DEGENERATE
             * PPS (a zeros-only body): emitting its header alone
             * poisons the decoder - validate the body (pps_id and
             * sps_id ue must both parse) and DROP it otherwise; the
             * decoder keeps the previous chain's PPS. */
            while (body > 0 && p[s + 4 + body - 1] == 0)
                body--;
            {
                size_t bp = 0;
                unsigned a, b;
                if (body < 2) {
                    i = e;
                    continue;
                }
                a = sps_read_ue(p + s + 4, body, &bp);
                b = sps_read_ue(p + s + 4, body, &bp);
                if (a == 0xFFFF || b == 0xFFFF || bp > body * 8) {
                    if (f2f_trace())
                        fprintf(stderr, "rec: degenerate PPS dropped\n");
                    i = e;
                    continue;
                }
            }
            e = s + 4 + body;
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
        } else if (nt == 9) {          /* AUD: decoders don't need it, and
                                         * it is new in this era's records -
                                         * drop it */
            i = e;
            continue;
        } else {                       /* SEI/junk */
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

/* One delivered record -> the output. Video goes through the NAL walk
 * + chain gate (emit_record, which counts its own stats). Audio: the
 * record payload is already one complete ADTS frame (verified on the
 * snapshots: ff f9 sync, frame_length == record len) - pass it
 * through whole, whole-or-drop like a P slice. */
static int emit_delivered(const uint8_t *p, uint32_t len, uint16_t type)
{
    int r;

    if (!audio_mode)
        return emit_record(p, len, type);
    if (out_fd >= 0) {
        r = write_whole_fd(out_fd, p, len);
    } else {
        r = (fwrite(p, 1, len, out) == (size_t)len) ? 0 : -2;
        if (out)
            fflush(out);
    }
    if (r == 0) {
        g_nals++;
        g_bytes += len;
    } else if (r == -1) {
        g_drops++;
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
    /* Stall watchdog state: "ring flowing but nothing consumed" */
    time_t last_emit = 0;
    uint32_t last_valid = 0;
    int stall_anchors = 0;

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
                ring_copy(off, &h, sizeof(h));
                if (!magic_ok(h.magic))
                    break;             /* torn/stale: stop, retry next poll */
                if (26 + h.len > left || h.len > MAXPAY)
                    break;
                if (h.seq == newest)
                    break;             /* the newest record may be mid-copy */
                if (entry_wanted(slot_cursor, g_filter, &h)) {
                    /* Live-edge jump: when the walk's first match is
                     * far ahead of the cursor (a client stall, a slow
                     * drain), serving the backlog would trail the
                     * live edge by the stall's whole duration - the
                     * backlog can never be closed. tserver's
                     * streaming loop uses read_latest for the same
                     * reason: drop it and jump. The chain gate
                     * re-arms at the next chain (~2.5 s). */
                    if (h.seq - (slot_cursor + 1u) > 100u) {
                        if (f2f_trace())
                            fprintf(stderr,
                                    "jump: cursor %u -> %u (%u skipped)\n",
                                    slot_cursor, h.seq,
                                    h.seq - (slot_cursor + 1u));
                        slot_cursor = h.seq - 1;
                        continue;      /* re-scan from the tail: the
                                          chain records between the
                                          cursor and here are skipped,
                                          the gate re-arms below */
                    }
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
            if (!got) {
                /* era reset: the writer's seq dropped (a new counter
                 * era, the app resets it mid-uptime) - the cursor
                 * belongs to the dead epoch and no record will ever
                 * match it again. Re-anchor at the newest seq. */
                if (newest < slot_cursor &&
                    slot_cursor - newest > 10000u) {
                    slot_cursor = newest;
                    slot_wr_u32(SLOT_OFF(slot_k) + 8, newest);
                    if (f2f_trace())
                        fprintf(stderr,
                                "era: cursor re-anchored to %u\n", newest);
                }
                slot_wr_u32(SLOT_OFF(slot_k), 0);  /* nothing for us now */
            }
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

        /* Stall watchdog: the writer is flowing (valid moves) but we
         * have consumed nothing for a while. Re-anchor the cursor at
         * the newest seq - covers magic-era drift and seq-era resets
         * the walk's own checks missed (e.g. a prefix change outside
         * the 0x6aXX family). If consecutive re-anchors bring no
         * records back, the ring carries no records at all (the app
         * flipped to raw Annex-B, or a torn region never heals): exit
         * so start-rtsp.sh's watcher respawns us with a fresh claim. */
        if (last_emit) {
            uint32_t v = hdr_u32(HDR_VALID);
            if (v != last_valid) {
                if (time(NULL) - last_emit >= STALL_SECS) {
                    fprintf(stderr,
                            "stall: %ld s without consuming a record while "
                            "the ring flows (valid %u -> %u, seq %u) - "
                            "re-anchoring\n",
                            (long)(time(NULL) - last_emit), last_valid, v,
                            hdr_u32(HDR_SEQ));
                    slot_cursor = hdr_u32(HDR_SEQ);
                    slot_wr_u32(SLOT_OFF(slot_k) + 8, slot_cursor);
                    last_emit = time(NULL);
                    if (++stall_anchors >= MAX_STALL_REANCHOR) {
                        fprintf(stderr,
                                "stall: no records after %d re-anchors - "
                                "exiting for the watcher to respawn\n",
                                stall_anchors);
                        release_slot();
                        return;
                    }
                }
                last_valid = v;
            }
        }

        if (got) {
            last_emit = time(NULL);
            stall_anchors = 0;
        }

        if (!got) {
            usleep(POLL_MS * 1000);
            continue;
        }

        if (one_shot && budget && --budget == 0)
            one_shot_done = 1;
        if (emit_delivered(payload, cur_len, cur_type) == -2) {
            /* A broken pipe (the readers briefly vanished - e.g. the
             * server between clients) must not kill the reader: reopen
             * the write end and drop this one NAL. */
            if (out_fd >= 0) {
                fprintf(stderr, "fifo: reopen after %s\n", strerror(errno));
                close(out_fd);
                out_fd = open(g_fifo_name, O_WRONLY | O_NONBLOCK);
                if (out_fd < 0)
                    perror("reopen");
            } else {
                perror("write");
                return;
            }
        }
        if (one_shot_done)
            return;
    }
}

/* ---- slot claim + the join scan (lock-free) ----
 * Slots 10..14 are ours (the stock consumers use 0, 1, 2, 16). Claim
 * the first with a zero filter - or a filter left by our own previous
 * run in the same mode (stale cursor, harmless to re-seed). A slot
 * holding the OTHER mode's filter (a live video producer while we are
 * audio, or vice versa) is occupied and skipped. Video seeds the
 * cursor at the newest 0x0422 chain start so the first emission is a
 * complete SPS->PPS->IDR chain; audio has no chain, so it seeds 0
 * (fresh reader, consume from the tail) and the live-edge jump lands
 * it one record behind the head on the first poll. The slot writes
 * are plain aligned stores; the writer reads the filter under its own
 * lock and never reads a cursor, so there is nothing to race on. */
static int claim_slot(int want_slot)
{
    int k;
    int first = want_slot >= 0 ? want_slot : 10;
    int last = want_slot >= 0 ? want_slot : 14;

    for (k = first; k <= last; k++) {
        uint32_t filt;
        uint32_t best = 0;

        filt = hdr_u32(SLOT_OFF(k) + 12) & 0xFFFF;
        if (filt != 0 && filt != g_filter)
            continue;                  /* occupied by someone else */
        if (!audio_mode) {
            /* seed the cursor: the newest 0x0422 with at least 4
             * records after it (its IDR is then written too: a
             * complete chain) */
            uint32_t off = hdr_u32(HDR_TAIL);
            uint32_t left = hdr_u32(HDR_VALID);
            uint32_t newest = hdr_u32(HDR_SEQ);
            while (left > 26) {
                rec_hdr h;
                ring_copy(off, &h, sizeof(h));
                if (!magic_ok(h.magic))
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
            /* cursor = the chain start MINUS one: the 0x0422 itself
             * must be delivered (entry_wanted wants seq >= cursor+1)
             * so the chain gate can open on it */
            if (best)
                best--;
        }
        slot_wr_u32(SLOT_OFF(k) + 8,
                    best ? best : (audio_mode ? 0u : hdr_u32(HDR_SEQ)));
        slot_wr_u32(SLOT_OFF(k), 1);          /* pending: poll starts */
        slot_cursor = best ? best : (audio_mode ? 0u : hdr_u32(HDR_SEQ));
        /* publish the filter: a single aligned u16 store */
        {
            uint16_t f = g_filter;
            memcpy((void *)(buf + SLOT_OFF(k) + 12), &f, 2);
        }
        slot_k = k;
        fprintf(stderr, "slot %d claimed (filter 0x%04x, cursor %u)\n",
                k, g_filter, slot_cursor);
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
    int fifo_set = 0;
    int opt;
    int fd;
    struct stat st;
    pthread_t unlock_thread;

    /* run as f2f_audio == audio mode (the deployed copy's name; comm
     * stays short enough for pidof, see start-rtsp.sh) */
    {
        const char *pn = strrchr(argv[0], '/');
        pn = pn ? pn + 1 : argv[0];
        if (strcmp(pn, "f2f_audio") == 0)
            audio_mode = 1;
    }

    while ((opt = getopt(argc, argv, "af:o:ns:h")) != -1) {
        switch (opt) {
        case 'a': audio_mode = 1; break;
        case 'f': fifo_name = optarg; g_fifo_name = optarg; fifo_set = 1; break;
        case 'o': out_file = optarg; break;
        case 'n': one_shot = 1; break;
        case 's': want_slot = atoi(optarg); break;
        default:
            fprintf(stderr,
                    "usage: %s [-a] [-f FIFO] [-o FILE] [-n] [-s SLOT]\n",
                    argv[0]);
            return 1;
        }
    }

    if (audio_mode) {
        g_filter = FILTER_AUDIO;
        g_purge_trim = PURGE_TRIM_AUDIO;
        if (!fifo_set) {
            fifo_name = DEFAULT_AUDIO_FIFO;
            g_fifo_name = DEFAULT_AUDIO_FIFO;
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
