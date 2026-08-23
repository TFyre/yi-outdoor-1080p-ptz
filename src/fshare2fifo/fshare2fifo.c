/*
 * fshare2fifo - forward the H.264 stream from the YI FH8626V100 shared
 * frame buffer (/dev/shm/fshare_frame_buf) to a fifo or file.
 *
 * The stock app (rmm) continuously writes encoded H.264 into the shared
 * buffer and bumps a frame counter at offset 0x18. The buffer interleaves
 * TWO streams: a high-res one (1920x1088, SPS level 0x29) and a low-res one
 * (640x368, level 0x16), plus the app's own table entries (00 00 01 c0
 * records with size/counter/timestamp fields) and raw low-res slice data
 * without start codes. This tool emits only the high-res stream; the ring
 * contents were reverse-engineered from live captures (tools/ringdiff.py,
 * analysis snapshots):
 *
 *   - every P-slice NAL body starts with 9a 00 (first_mb=0, slice_type),
 *     every IDR slice with 88 80 - checked against 223 live slices /
 *     9 IDRs, zero exceptions
 *   - high-res GOP head = SPS+PPS+SPS+PPS+IDR, all level 0x29
 *   - the second stream's SPS is level 0x16, so IDRs are only emitted
 *     inside a chain that started with a target-resolution SPS (chain
 *     gating in want_nal)
 *   - the two streams' P-slices INTERLEAVE, share the 9a 00 shape AND
 *     share frame numbers; the target frames are multi-slice (big main
 *     >= 600 bytes + small twin repeating the main's frame number), the
 *     other stream's slices are all small with frame number main+0x1c.
 *     The pic_order_cnt byte is NOT a usable discriminator (it differed
 *     between streams in one capture era, 0x90 vs 0x91, and became
 *     identical 0x92 in another). Mixing the streams corrupts every P
 *     frame; the size + twin-frame-number rules filter them apart.
 *   - the 00 00 01 c0 table entries carry the app's frame counter and are
 *     the newest-write beacon (head_estimate), replacing the unreliable
 *     header queue slots
 *
 * Before forwarding anything it waits for a complete SPS->PPS->IDR chain
 * of the target stream in the ring, so clients joining the stream decode
 * a clean picture from their very first frame instead of mid-GOP
 * artifacts. NAL start codes are written as 4-byte (00 00 00 01) - the
 * ring uses 3-byte codes, and LIVE555's framer only syncs on 4-byte ones.
 *
 * The walk is COMPLETENESS-GATED: a NAL is emitted only when its end
 * lies at least NAL_FLOOR before the writer's newest position signal
 * (the newest c0 checkpoint entry, or the header queue slots when the
 * app stops writing checkpoints mid-uptime - both modes verified live),
 * so every emitted byte is provably final. The age at emit is the
 * NAL's own write time (one frame) plus the head signal's lag -
 * bitrate-independent, unlike a fixed byte gap. A long fifo block is
 * just backpressure (the join/drop sprint draining at the client's
 * rate), so there is deliberately NO time-based lap re-sync: the old
 * tick check fired on exactly those blocks and looped forever
 * (sprint -> block -> re-sync -> sprint). If the writer laps the walk,
 * the gate stalls it until the chain gate re-arms at the next IDR. A
 * join (or the MAX_LAG escape) emits the newest chain, then JUMPS the
 * walk to the gate line - the mid-GOP backlog is dropped, because the
 * drain runs at the writer's own rate and the backlog could never be
 * closed by emitting it.
 *
 * Based on the design of h264grabber.c (GPL-3) by roleoroleo / yi-hack-v5.
 *
 * Usage: fshare2fifo [-f FIFO] [-o FILE] [-n]
 *   default output: fifo /tmp/h264_high_fifo
 *   -n: one-shot sweep (dump up to 256 NALs, then exit) - for captures
 *       and offline tests against a static buffer snapshot
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
#define FRAME_COUNTER_OFFSET 0x18
/* The writer ticks the frame counter ~43/s. A fifo write that blocks
 * for this many ticks (>= ~1.2 s) means the client stalled; we re-sync
 * BEFORE the writer can lap our read position (the lap needs ~3 s at
 * the worst-case bitrate). The threshold sits ABOVE the server's
 * natural drain bursts: with the fifo at 256 KB those last <= ~0.6 s,
 * so normal paced writes never trip it. A larger fifo + higher
 * threshold left a gray zone (1.6-3 s blocks) where the writer laps
 * us without a re-sync - full-frame conceal storms. */
/* Completeness floor: a NAL is emitted only when its END lies at least
 * this far before the writer's newest position signal (see drain_round),
 * on top of the NAL's own length. A fixed byte gap is bitrate-blind:
 * 192 KB is ~0.4 s of content at the motion bitrate but ~27 s on an
 * ultra-static scene - the observed 13 s latency. Gating by the NAL's
 * own length makes the age one frame at ANY bitrate. */
#define NAL_FLOOR 128
/* If the walk ever falls this far behind the head (a client stall
 * longer than the gate can absorb), the backlog cannot be closed by
 * sprinting - the fifo drain runs at the writer's own rate - so
 * re-join at the newest chain and JUMP to the gate line instead of
 * dragging minutes of doomed content. */
#define MAX_LAG (768u * 1024)
/* Backward moves of the head signal below this are round-robin jitter
 * (a few KB), not a ring wrap (~1.7 MB); clamp them in update_head. */
#define JITTER_MAX (64u * 1024)
#define DEFAULT_FIFO "/tmp/h264_high_fifo"
#define SCAN_START 0x100          /* skip the small header */
#define POLL_MS 10
#define NAL_BUF_MAX 256           /* SPS/PPS are tiny; slices pass through */
#define MIN_SLICE 8               /* sanity floor for slice bodies */

static volatile uint8_t *buf;      /* mmap of the shared buffer */
static size_t buf_size;
static size_t stream_base;         /* first emitted NAL position in the ring */
static FILE *out;                  /* file output (-o FILE) */
static int out_fd = -1;            /* fifo output, blocking */

/* Resolution filtering: emit only the largest stream seen so far. */
static unsigned target_w, target_h;   /* dims of the stream being emitted */
static int emit_mode;                 /* 1 when the current NAL belongs to it */
/* Chain state across the NAL walk: 0 = outside a chain, 1 = accepted SPS
 * seen, 2 = accepted SPS+PPS seen. An IDR completes (and resets) it. */
static int have_sps;
/* 0 = normal walk, 1 = freshly (re-)joined and emitting the join chain,
 * 2 = the chain's IDR is out: drain_forever jumps the walk to the gate
 * line, dropping the un-closable mid-GOP backlog. */
static int just_joined;
/* Slice discriminator: the ring carries two interleaved streams whose
 * slices share the same 9a 00 header shape. The app's own frame table
 * (the 00 00 01 c0 entries, one per consumed frame) carries a TYPE
 * field at entry offset 24: 0x0400 = high-res video, 0x0800 = low-res
 * video, 0x0100 = audio - the same flags as roleoroleo's
 * yi-hack-Allwinner-v2 frame_header. Every entry-framed high-res slice
 * in the captures is >= 955 bytes (P and B frames alike, 1.2-25 KB)
 * while the low-res stream's slices are <= 475 bytes, so size splits
 * the streams: emit slices >= 700 bytes only. The small slices are ALL
 * the low-res stream's (a "twin slice" theory was tried and reverted:
 * those small slices are type 0x0800 per the table, and when one's
 * frame number happened to match a preceding main it leaked low-res
 * data into the 1080p stream, breaking every subsequent P frame). The
 * pic_order_cnt byte is NOT usable: it differed between the streams in
 * one capture era (0x90 vs 0x91) and became identical (0x92) in
 * another. */
#define MIN_MAIN 700

/* Fifo purger thread: holds the read end open (a fifo writer with no
 * reader gets EPIPE; the open also completes the reader/writer open
 * dance with the producer's write open below) and, when the fifo is
 * IDLE-FULL, discards the OLD bytes keeping a fresh ~224 KB tail - big
 * enough for a complete SPS+PPS+IDR chain. Without this, a fifo left
 * full from the last session serves the next client minutes-old
 * content (the observed "starts fresh, then jumps back" connect
 * wobble). A slow live client is trimmed the same way: it conceals the
 * gap and comes back live instead of dragging the backlog.
 *
 * It must never consume the stream head while the fifo is NOT full -
 * an early version ate the first 1024 bytes (the SPS+PPS+IDR head)
 * and made every client join mid-GOP. */
static void *fifo_purger_thread(void *arg)
{
    const char *fifo_name = arg;
    int fd = open(fifo_name, O_RDONLY);   /* blocking: completes the dance */
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

static int is_nal_type(uint8_t b)
{
    switch (b & 0x1F) {
    case 1:  /* non-IDR slice */
    case 5:  /* IDR slice */
    case 6:  /* SEI */
    case 7:  /* SPS */
    case 8:  /* PPS */
    case 9:  /* AUD */
        return 1;
    default:
        return 0;
    }
}

/* Find the next start code (00 00 01) at or after *pos, wrapping at the
 * end of the buffer. If "typed", only start codes of accepted NAL types
 * match; otherwise any byte counts. Returns 1 and sets *pos, or 0 if the
 * whole ring was scanned without a hit. */
static int next_sc(size_t *pos, int typed)
{
    size_t p = *pos;
    size_t scanned = 0;

    while (scanned < buf_size) {
        if (p + 4 > buf_size) {          /* wrap */
            p = 0;
        }
        if (p + 4 <= buf_size &&
            buf[p] == 0x00 && buf[p + 1] == 0x00 && buf[p + 2] == 0x01 &&
            (!typed || is_nal_type(buf[p + 3]))) {
            *pos = p;
            return 1;
        }
        p++;
        scanned++;
    }
    return 0;
}

/* Minimal H.264 SPS parser: extract picture dimensions (Main/Baseline and
 * the common High profiles). Returns 1 and sets w,h, or 0. The NAL
 * passed in starts at the NAL header byte (no start code). */
static int sps_read_bit(const uint8_t *s, size_t slen, size_t *pos)
{
    int b;
    if (*pos >= slen * 8)
        return 0;
    b = (s[*pos >> 3] >> (7 - (*pos & 7))) & 1;
    (*pos)++;
    return b;
}

static int f2f_trace(void)
{
    return getenv("F2F_TRACE") != NULL;
}

/* One-per-second diagnostics (F2F_AGELOG=1): where the walk sits relative
 * to the app's write head, the emission rate, and how long fifo writes
 * block us - the three numbers that explain a latency drift. Kept out of
 * the emit path when disabled so production runs pay nothing. */
static int f2f_agelog(void)
{
    return getenv("F2F_AGELOG") != NULL;
}

static volatile unsigned g_nals, g_bytes;    /* emitted since the last line */
static volatile unsigned g_block_ticks;      /* ticks spent inside fifo writes */
static volatile unsigned g_max_block;        /* longest single write (ticks) */
static volatile unsigned g_drops;            /* NALs dropped on a full fifo */

static unsigned sps_read_ue(const uint8_t *s, size_t slen, size_t *pos)
{
    unsigned z = 0, v = 0, i;
    while (*pos < slen * 8 && sps_read_bit(s, slen, pos) == 0)
        z++;
    for (i = 0; i < z && i < 24; i++)
        v = (v << 1) | (unsigned)sps_read_bit(s, slen, pos);
    return z > 24 ? 0xFFFFU : ((1u << z) - 1 + v);
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
    /* strip emulation prevention bytes (00 00 03 -> 00 00) */
    for (i = 0; i < len && slen < sizeof(stripped); i++) {
        if (slen >= 2 && stripped[slen - 1] == 0 && stripped[slen - 2] == 0 &&
            d[i] == 3)
            continue;
        stripped[slen++] = d[i];
    }
    if (slen < 4)
        return 0;

    /* the NAL starts at its header byte: skip nal header + profile_idc +
     * constraints + level_idc (4 bytes = 32 bits) */
    profile = stripped[1];
    pos = 32;
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
        (void)sps_read_bit(stripped, slen, &pos); /* qpprime_y_zero_transform_bypass */
        if (sps_read_bit(stripped, slen, &pos)) { /* seq_scaling_matrix_present */
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
    (void)sps_read_bit(stripped, slen, &pos); /* gaps_in_frame_num_value_allowed */
    SPS_UE("pic_width_in_mbs_minus1", val);
    SPS_UE("pic_height_in_map_units_minus1", i);
    if (val == 0xFFFF || i == 0xFFFF || val > 300 || i > 300)
        return 0;                     /* implausible - bad parse */
    *w = (val + 1) * 16;
    *h = (i + 1) * 16;
    if (sps_read_bit(stripped, slen, &pos) == 0)  /* frame_mbs_only_flag */
        *h *= 2;
    return 1;
}

/* Copy the NAL body (bytes after the 3-byte start code) from ring
 * position p up to the next start code e, handling the ring wrap.
 * Returns the length copied, or 0 if it does not fit in max. */
static size_t copy_nal(size_t p, size_t e, uint8_t *dst, size_t max)
{
    size_t len = (e > p) ? (e - p - 3) : ((buf_size - p - 3) + e);
    if (len > max)
        return 0;
    if (e > p) {
        memcpy(dst, (const void *)(buf + p + 3), len);
    } else {
        memcpy(dst, (const void *)(buf + p + 3), buf_size - p - 3);
        memcpy(dst + buf_size - p - 3, (const void *)buf, e);
    }
    return len;
}

/* Copy up to n bytes of the NAL body from p, starting at the NAL header
 * byte (p + 3), handling the ring wrap. Returns the count copied. Unlike
 * copy_nal this never refuses a long NAL - callers use it for small
 * header prefixes of arbitrarily large slices. */
static size_t copy_nal_head(size_t p, uint8_t *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        size_t q = p + 3 + i;
        dst[i] = buf[q < buf_size ? q : q - buf_size];
    }
    return n;
}

/* Decide whether the NAL whose start code is at ring position p (ending at
 * the next start code e) belongs to the stream we emit, and maintain the
 * SPS->PPS->IDR chain state used to gate IDRs and PPS (the low-res stream
 * uses the same code shapes but its SPS is level 0x16, so its chain never
 * completes and its IDRs never pass).
 *
 * Empirical slice-header shapes (verified on live captures, zero
 * exceptions): high/low P-slices start 41 9a 00 ..., IDR slices start
 * 65 88 80 ...; accidental 00 00 01 codes inside entropy data never carry
 * those prefixes, which is what keeps the stream clean.
 */
static int want_nal(size_t p, size_t e)
{
    uint8_t t = buf[p + 3] & 0x1F;
    size_t body = (e > p) ? (e - p - 3) : (buf_size - p - 3 + e);

    if (!is_nal_type(buf[p + 3]))
        return 0;                     /* table entries, junk */
    if (t == 1) {
        uint8_t hd[8];                /* NAL header + first 6 body bytes */
        if (!emit_mode || body < MIN_SLICE)
            return 0;
        if (copy_nal_head(p, hd, 7) < 7)
            return 0;
        if (hd[1] != 0x9a || hd[2] != 0x00)
            return 0;                 /* not a real slice of this camera */
        if (body < MIN_MAIN)
            return 0;                 /* the low-res stream's slice */
        have_sps = 0;                 /* a slice breaks the chain */
        return 1;
    }
    if (t == 5) {
        uint8_t hd[3];
        if (copy_nal_head(p, hd, 3) < 3)
            return 0;
        if (hd[1] != 0x88 || hd[2] != 0x80)
            return 0;
        if (!emit_mode || have_sps != 2)
            return 0;                 /* IDR outside a target chain */
        have_sps = 0;                 /* the chain is consumed */
        return 1;
    }
    if (t == 7) {
        unsigned w = 0, h = 0;
        uint8_t sps_buf[NAL_BUF_MAX];
        size_t len;
        len = copy_nal(p, e, sps_buf, sizeof(sps_buf));
        if (f2f_trace())
            fprintf(stderr, "SPS at 0x%zx (%zu bytes):\n", p, len);
        if (len == 0 || !parse_sps_dims(sps_buf, len, &w, &h)) {
            have_sps = 0;             /* unparseable: not a real SPS */
            return 0;
        }
        if ((uint64_t)w * h > (uint64_t)target_w * target_h) {
            target_w = w;
            target_h = h;
        }
        if (target_w != 0 && w == target_w && h == target_h) {
            have_sps = 1;
            return 1;
        }
        have_sps = 0;                 /* the other stream's SPS */
        return 0;
    }
    if (t == 8) {                     /* PPS only inside a target chain */
        if (!emit_mode || have_sps != 1)
            return 0;
        have_sps = 2;
        return 1;
    }
    /* SEI/AUD: legal inside a chain, neutral outside */
    return emit_mode;
}

static uint32_t frame_counter(void)
{
    uint32_t v;
    memcpy(&v, (const void *)(buf + FRAME_COUNTER_OFFSET), sizeof(v));
    return v;
}

/* The app's newest write position: the newest 00 00 01 c0 table entry
 * (its counter field ticks with the frame counter). The entries carry the
 * frame counter at +8 and a 0x6a8a.. magic at +12, so an accidental code
 * match in slice data cannot fake one. Falls back to the max of the
 * header queue slots when no entry is present (record-mode rings).
 * Sets *ctr_out (when given) to the entry's counter. */
static uint32_t head_estimate(uint32_t *ctr_out)
{
    uint32_t fctr = frame_counter();
    uint32_t best_pos = 0, best_ctr = 0;
    size_t p;

    for (p = 0; p + 31 <= buf_size; p++) {
        uint32_t ctr, magic;
        if (buf[p] != 0x00 || buf[p + 1] != 0x00 || buf[p + 2] != 0x01 ||
            buf[p + 3] != 0xc0)
            continue;
        memcpy(&ctr, (const void *)(buf + p + 8), sizeof(ctr));
        memcpy(&magic, (const void *)(buf + p + 12), sizeof(magic));
        if ((magic & 0xffff0000u) != 0x6a8a0000u)
            continue;
        if (ctr <= fctr && ctr > best_ctr) {
            best_ctr = ctr;
            best_pos = (uint32_t)p;
        }
    }
    if (best_ctr) {
        if (ctr_out)
            *ctr_out = best_ctr;
        return best_pos;
    }

    /* fallback: round-robin queue of the last write positions at
     * 0x04/0x0C/0x10; the head is the newest (max) of the three */
    {
        uint32_t v, h = 0;
        unsigned off;
        for (off = 0x04; off <= 0x10; off += 4) {
            memcpy(&v, (const void *)(buf + off), sizeof(v));
            if (v < buf_size && v > h)
                h = v;
        }
        if (ctr_out)
            *ctr_out = 0;
        return h;
    }
}

/* Tracked writer head: the newest complete 00 00 01 c0 entry, kept by a
 * bounded FORWARD scan from the last known one (entries only move
 * forward with the frame counter, so a full-ring rescan per NAL is never
 * needed). The walk itself is position-gated and can never discover
 * entries past its own stop line; this peek does. Falls back to a
 * full-ring scan when the forward scan makes no progress for ~2 s
 * (entry-less record-mode rings, or a stale track after a mode change). */
#define HEAD_SCAN_LIM (96u * 1024)
static uint32_t head_pos;              /* newest entry's position (0 = unknown) */
static uint32_t head_ctr;              /* its frame counter */
static unsigned head_misses;           /* forward scans without progress */
static uint32_t head_last;             /* last returned head (jitter clamp) */
static uint32_t head_live_fctr;        /* frame counter at the last advance */

/* The ring-diff sampler's writer position (diff_thread below):
 * authoritative in every mode once the sampler has produced it. */
#define DATA_BASE 0x100               /* below this is header/slots only */
static uint8_t *shadow;               /* last sampled copy of the ring */
static volatile uint32_t diff_head;   /* newest write position seen */

static uint32_t update_head(void)
{
    uint32_t fctr = frame_counter();
    size_t p = head_pos;
    size_t scanned = 0;

    if (head_pos != 0) {
        while (scanned < HEAD_SCAN_LIM) {
            if (p + 34 > buf_size)
                p = 0;
            if (p + 34 <= buf_size &&
                buf[p] == 0x00 && buf[p + 1] == 0x00 && buf[p + 2] == 0x01 &&
                buf[p + 3] == 0xc0) {
                uint32_t ctr, magic;
                memcpy(&ctr, (const void *)(buf + p + 8), sizeof(ctr));
                memcpy(&magic, (const void *)(buf + p + 12), sizeof(magic));
                if ((magic & 0xffff0000u) == 0x6a8a0000u &&
                    ctr > head_ctr && ctr <= fctr) {
                    head_ctr = ctr;
                    head_pos = (uint32_t)p;
                    head_misses = 0;
                }
            }
            p++;
            scanned++;
        }
    }
    if (head_pos == 0 || ++head_misses > 60) {
        uint32_t ctr = 0;
        head_pos = head_estimate(&ctr);
        if (ctr)
            head_ctr = ctr;
        head_misses = 0;
    }

    /* The diff sampler's head is authoritative in every mode; the c0
     * and slot signals above are the seed (and the fallback if the
     * sampler cannot allocate its shadow). */
    if (diff_head != 0)
        return diff_head;

    /* Stale c0 stream: the app switches ring modes mid-uptime and stops
     * writing checkpoints (verified live: the entries vanish while the
     * frame counter keeps ticking). Fall back to the header queue slots
     * at 0x04/0x08/0x0C/0x10 - the writer's own round-robin position
     * queue; the max of the four is the newest write. The slot head is
     * returned WITHOUT touching head_pos/head_ctr, so the c0 tracking
     * resumes automatically if the app returns to the steady-state
     * mode. 300 ticks (~6.7 s) is far beyond any steady-state entry gap
     * (checkpoints come ~5 frames apart). */
    {
        uint32_t h = head_pos;
        if (head_pos != 0 && frame_counter() - head_ctr > 300) {
            uint32_t v, m = 0;
            unsigned off;
            for (off = 0x04; off <= 0x10; off += 4) {
                memcpy(&v, (const void *)(buf + off), sizeof(v));
                if (v < buf_size && v > m)
                    m = v;
            }
            h = m;   /* 0 when even the slots are dead: heartbeat gate */
        }
        /* The slot signal jitters a few KB BACKWARD between updates
         * (the queue's max is not strictly monotonic). A raw backward
         * step would flip the walk's gate open - the wrapped distance
         * becomes ~1.7 MB - and the walk sprints a whole ring lap.
         * Clamp small backward moves; a real lap wraps by ~1.7 MB. */
        if (h != 0 && head_last != 0 && h < head_last &&
            head_last - h < JITTER_MAX)
            h = head_last;
        if (h != 0 && h != head_last) {
            head_last = h;
            head_live_fctr = frame_counter();
        }
        /* Dead signal: the counter ticks but the position signal has
         * not moved for ~13 s (verified live: the slots freeze when
         * the writer reaches the buffer end). A frozen nonzero head
         * deadlocks the walk at the gate; the heartbeat gate at least
         * keeps the stream moving until the signal recovers. */
        if (head_last != 0 &&
            frame_counter() - head_live_fctr > 600)
            return 0;
        return h;
    }
}

/* ---- writer position sampler (ring diff) ---- */
/* The app's position signals (c0 checkpoints, header slots) die when
 * the ring switches modes mid-uptime (verified live: the slots froze at
 * the buffer end while the encoder kept running). The writer's true
 * position is observable anyway: it writes contiguously, so the TOP of
 * the newest changed bytes in a periodic diff of the ring IS the write
 * head, in every mode. The sampler keeps a shadow copy and diffs the
 * window ahead of the last known head every 250 ms; the completeness
 * gate then runs against a position never more than ~110 KB stale
 * (0.25 s at the peak bitrate). Only the header (below 0x100) is
 * excluded: the ring's data base itself varies by mode/era (measured
 * below 0x224b0), so everything above the header must be scanned. */
#define DIFF_WIN (512u * 1024)       /* > max bytes per sample at peak rate */

static void *diff_thread(void *arg)
{
    int first = 1;
    uint32_t base = DATA_BASE;
    (void)arg;

    usleep(250 * 1000);
    while (1) {
        uint32_t q = base;
        size_t left = first ? buf_size : DIFF_WIN;
        uint32_t run1 = 0;
        unsigned changed = 0;
        int run = 0;
        size_t i;

        while (left > 0) {
            size_t n;
            if (q >= buf_size)
                q = 0;
            if (q < DATA_BASE)
                q = DATA_BASE;
            n = buf_size - q;
            if (n > left)
                n = left;
            for (i = 0; i < n; i++) {
                if (buf[q + i] != shadow[q + i]) {
                    /* run1 = the LAST changed byte in scan order: the
                     * writer writes contiguously in time, so this is
                     * its newest position even when coincidentally
                     * unchanged bytes split the span into fragments. */
                    run = 1;
                    run1 = q + i;
                    changed++;
                }
            }
            /* Refresh the shadow for the WHOLE scanned chunk now: the
             * window wraps past the buffer end, and without this the
             * wrapped portion compares against a startup-era shadow -
             * false changes there yanked the head backward by hundreds
             * of KB and made the walk chase garbage. */
            memcpy(shadow + q, (const void *)(buf + q), n);
            q += (uint32_t)n;
            left -= n;
        }
        if (run) {
            if (f2f_trace())
                fprintf(stderr,
                        "diff: base=0x%x run1=0x%x changed=%u\n",
                        (unsigned)base, (unsigned)run1, changed);
            base = (run1 + 1 >= buf_size) ? DATA_BASE : run1 + 1;
            diff_head = base;
        }
        first = 0;
        usleep(250 * 1000);
    }
    return NULL;
}

/* Distance from pos to the (cached) head position. */
static size_t dist_to_head_cached(size_t pos, uint32_t head)
{
    uint32_t h = head % (uint32_t)buf_size;
    if (h >= pos)
        return h - pos;
    return (size_t)h + buf_size - pos;
}

/* Write all n bytes to fd, looping over partial writes. A full fifo
 * retries for up to ~10 ms before giving up: equal-rate pacing (the
 * client plays at wall-clock speed, the walk writes at the writer's
 * speed) fills the fifo TRANSIENTLY all the time, and those absorb as
 * micro-stalls instead of dropping a NAL (a dropped NAL is a concealed
 * frame). Only a REAL stall returns -1, which drops the NAL and keeps
 * the walk glued to the writer. The budget must stay small: with no
 * client every NAL burns it, and a long budget makes the walk's
 * rounds crawl far behind the writer.
 * Returns 0 on success, -1 (fifo full past the retry budget), or -2
 * on a hard error. */
static int write_all_fd_pace(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    int tries = 0;
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                tries < 5) {
                tries++;
                usleep(2000);
                continue;
            }
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
        }
        tries = 0;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* A complete NAL is copied out of the ring BEFORE the (potentially
 * blocking) fifo write: while we are blocked, the writer laps the ring
 * and would overwrite the bytes mid-write, truncating e.g. a 130 KB
 * IDR - one truncated keyframe breaks every dependent P frame until
 * the next clean one (full-frame conceal storms). Writing a private
 * copy also lets the lap check run only after the NAL is complete, so
 * a re-sync never abandons a half-written NAL either. */
#define NAL_COPY_MAX (192u * 1024)
static uint8_t nal_copy[NAL_COPY_MAX];

/* Emit one NAL (ring positions s..e) as 4-byte start code + body,
 * handling the ring wrap. Returns 0, -1 (would block), or -2 (error).
 * A long block here is just fifo backpressure: the walk sprinted a
 * backlog (e.g. the join chain) and the client drains at its own pace.
 * There is deliberately NO time-based lap re-sync: with the positional
 * gate the writer can never overwrite unread bytes, and if it does lap
 * the walk, the gate stalls the walk until the chain gate re-arms at
 * the next IDR (a bounded, self-healing mid-GOP conceals window). The
 * old tick check fired spuriously on exactly these sprint blocks and
 * looped forever: sprint -> block >= 1.1 s -> re-sync -> sprint. */
static int emit_nal(size_t s, size_t e)
{
    static const unsigned char start4[4] = {0x00, 0x00, 0x00, 0x01};

    if (out_fd >= 0) {
        uint32_t c0 = frame_counter();
        size_t len = (e > s) ? (e - s - 3) : (buf_size - s - 3 + e);
        int r;

        if (len > sizeof(nal_copy))
            return -2;                /* implausibly large NAL */
        len = copy_nal(s, e, nal_copy, sizeof(nal_copy));
        if (len == 0)
            return -2;
        r = write_all_fd_pace(out_fd, start4, sizeof(start4));
        if (r != 0)
            return r;
        r = write_all_fd_pace(out_fd, nal_copy, len);
        if (r != 0)
            return r;
        if (f2f_agelog()) {
            uint32_t c1 = frame_counter();
            g_nals++;
            g_bytes += len + 4;
            if (c1 > c0) {
                g_block_ticks += c1 - c0;
                if (c1 - c0 > g_max_block)
                    g_max_block = c1 - c0;
            }
        }
        return 0;
    }

    /* file mode: blocking stdio */
    if (fwrite(start4, 1, sizeof(start4), out) != sizeof(start4))
        return -2;
    if (e > s) {
        if (fwrite((const void *)(buf + s + 3), e - s - 3, 1, out) != 1)
            return -2;
    } else {
        if (fwrite((const void *)(buf + s + 3), buf_size - s - 3, 1, out) != 1)
            return -2;
        if (e > 0 && fwrite((const void *)buf, e, 1, out) != 1)
            return -2;
    }
    fflush(out);
    return 0;
}

/* Find a decodable start point: the most recent complete SPS->PPS->IDR
 * chain of the TARGET (largest) stream in the ring. Emitting from
 * anywhere else means clients join mid-GOP and show decoder artifacts
 * until the next keyframe; starting at the SPS of a complete chain gives
 * every new client a clean picture from its first frame. SEI/AUD NALs may
 * appear inside the chain. Returns 1 and sets *out, or 0 if no complete
 * chain is in the ring.
 *
 * Two passes: first learn the largest stream's dimensions (the ring holds
 * two interleaved streams and we must not lock onto the small one), then
 * pick the chain of that stream nearest the app's write head. */
static int find_idr_start(size_t *out)
{
    size_t p;
    size_t best = 0;
    size_t best_dist = (size_t)-1;
    size_t chain_start = 0;
    int found_chain = 0;
    int pass;
    uint32_t head = head_estimate(NULL);  /* once per call: the scan is costly */

    for (pass = 0; pass < 2; pass++) {
        size_t scanned = 0;
        p = SCAN_START;
        best = 0;
        best_dist = (size_t)-1;
        chain_start = 0;
        found_chain = 0;
        have_sps = 0;
        if (pass == 0)
            target_w = target_h = 0;
        emit_mode = (pass == 0) ? 0 : 1;

        while (scanned < buf_size) {
            size_t e;
            int want;
            uint8_t t;

            if (!next_sc(&p, 0))      /* any start code, wrapping */
                break;
            e = p + 4;
            if (!next_sc(&e, 0))
                break;
            t = buf[p + 3] & 0x1F;
            if (t == 7 && pass == 0) {
                /* learn dims only */
                unsigned w = 0, h = 0;
                uint8_t sps_buf[NAL_BUF_MAX];
                size_t len = copy_nal(p, e, sps_buf, sizeof(sps_buf));
                if (len && parse_sps_dims(sps_buf, len, &w, &h) &&
                    (uint64_t)w * h > (uint64_t)target_w * target_h) {
                    target_w = w;
                    target_h = h;
                }
                scanned += (e > p) ? (e - p) : (buf_size - p + e);
                p = e;
                continue;
            }
            want = 0;
            if (pass == 1) {
                if (t == 1) {
                    /* slices break the chain but are never emitted here */
                    have_sps = 0;
                } else {
                    want = want_nal(p, e);
                }
            }

            switch (t) {
            case 7:                       /* SPS restarts the chain */
                if (want)
                    chain_start = p;
                else
                    have_sps = 0;
                break;
            case 5:                       /* IDR completes the chain */
                if (want) {
                    /* prefer the chain nearest the app's write head: the
                     * backlog to catch up stays small, so the writer
                     * cannot lap and overwrite what we are about to emit */
                    size_t d = dist_to_head_cached(chain_start, head);
                    if (!found_chain || d < best_dist) {
                        best = chain_start;
                        best_dist = d;
                        found_chain = 1;
                    }
                }
                break;
            default:                      /* SEI/AUD/junk: neutral */
                break;
            }
            scanned += (e > p) ? (e - p) : (buf_size - p + e);
            p = e;
        }
    }
    if (target_w < 1280)
        return 0;
    if (!found_chain)
        return 0;
    *out = best;
    return 1;
}

/* Forward NAL units of the target stream, COMPLETENESS-GATED: a NAL
 * whose end lies at least NAL_FLOOR before the writer's newest position
 * signal (head) is provably complete - the writer signals its position
 * only after writing past it - and is emitted; anything closer may be
 * in flight and stops the walk. The gate's distance is the NAL's OWN
 * length, so the emitted content is ~one frame old at ANY bitrate (a
 * fixed byte gap was 0.4 s at the motion bitrate but ~27 s on an
 * ultra-static scene). Without a position signal the walk falls back to
 * the old heartbeat stop. With "force", the gate is skipped (one-shot
 * dumps of static snapshots).
 * Returns 0 or 2 (hard error). */
static int drain_round(size_t *pos, uint32_t head, int force)
{
    static uint32_t hb;               /* heartbeat for the no-head fallback */
    int i;

    for (i = 0; i < 256; i++) {
        size_t s = *pos, e;
        size_t len;
        uint8_t t;

        if (!next_sc(&s, 0))          /* nothing in the ring */
            return 0;
        e = s + 4;
        if (!next_sc(&e, 0) || e == s) /* no complete NAL yet */
            return 0;
        len = (e > s) ? (e - s - 3) : (buf_size - s - 3 + e);
        if (!force) {
            if (head != 0) {
                if (dist_to_head_cached(s, head) < len + 3 + NAL_FLOOR)
                    return 0;         /* may be in flight: wait */
            } else if (frame_counter() == hb) {
                /* no writer position signal (record-mode ring): fall
                 * back to the heartbeat stop */
                return 0;
            } else {
                hb = frame_counter();
            }
        }
        t = buf[s + 3] & 0x1F;
        if (want_nal(s, e)) {
            int r = emit_nal(s, e);
            if (r == -1) {
                /* Fifo full: DROP the NAL and keep walking. Blocking
                 * here instead would freeze the walk while the writer
                 * advances - the next client then unblocks us into a
                 * backlog sprint of old content, followed by a re-join
                 * at an older chain (the observed connect-time "starts
                 * fresh, then jumps back"). Dropping keeps the walk
                 * glued to the writer no matter how long the client
                 * stalls; the purger thread keeps the fifo tail fresh
                 * for the next join. */
                g_drops++;
            } else if (r != 0) {
                return r;
            } else if (t == 5 && just_joined == 1) {
                just_joined = 2;      /* join chain out: caller jumps */
            }
        }
        *pos = e;
    }
    return 0;
}

static void drain_forever(void)
{
    size_t pos = stream_base;
    time_t next_log = 0;
    int head_was_dead = 0;
    just_joined = 1;

    while (1) {
        uint32_t head = update_head();
        int r;

        if (f2f_agelog()) {
            time_t t = time(NULL);
            if (t >= next_log) {
                fprintf(stderr,
                        "age: pos=0x%zx head=0x%x dist=%zu nals=%u "
                        "bytes=%u block_ticks=%u max_block=%u drops=%u "
                        "fctr=%u\n",
                        pos, head, dist_to_head_cached(pos, head),
                        g_nals, g_bytes, g_block_ticks, g_max_block,
                        g_drops, frame_counter());
                g_nals = g_bytes = g_block_ticks = g_max_block =
                    g_drops = 0;
                next_log = t + 1;
            }
        }

        r = drain_round(&pos, head, 0);
        if (r == 2) {
            perror("write");
            return;
        }
        if (head == 0) {
            /* Dead position signal: the heartbeat gate ran the walk
             * blindly (see update_head); the positional machinery
             * below is meaningless until the signal recovers. */
            head_was_dead = 1;
            just_joined = 0;
            usleep(30 * 1000);
            continue;
        }
        if (head_was_dead) {
            /* The signal recovered while the walk roamed blind: its
             * position is meaningless - re-join at the newest chain. */
            head_was_dead = 0;
            if (find_idr_start(&pos)) {
                usleep(1000 * 1000);
                just_joined = 1;
                fprintf(stderr, "re-join: IDR start at 0x%zX\n", pos);
            } else {
                usleep(200 * 1000);
            }
            continue;
        }
        if (just_joined == 0 &&
            dist_to_head_cached(pos, head) > MAX_LAG) {
            /* Escaped the gate (a client stall longer than the gap):
             * re-join at the newest chain BEFORE emitting anything -
             * a backlog sprint here serves old content to a fresh
             * client. */
            if (find_idr_start(&pos)) {
                usleep(1000 * 1000);
                just_joined = 1;
                fprintf(stderr, "re-join: IDR start at 0x%zX\n", pos);
            } else {
                usleep(200 * 1000);
            }
            continue;
        }
        if (just_joined == 2) {
            /* The join chain is out: jump the walk to just behind the
             * head, dropping the mid-GOP backlog. The fifo drain runs
             * at the writer's own rate, so the backlog could never be
             * closed by emitting it - every sprint leaves the lag
             * exactly where it started. The decoder conceals the
             * dropped frames and goes live at ~one frame of age. */
            size_t j = head >= NAL_FLOOR ? head - NAL_FLOOR
                                         : buf_size + head - NAL_FLOOR;
            if (j > buf_size - 4)
                j = buf_size - 4;
            pos = j;
            just_joined = 0;
            fprintf(stderr, "jump: pos=0x%zx head=0x%x\n", pos, head);
        } else {
            /* Gated stop (or nothing to emit): wait for the writer to
             * advance past the gate line, then walk again. */
            usleep(30 * 1000);
        }
    }
}

int main(int argc, char **argv)
{
    const char *fifo_name = DEFAULT_FIFO;
    const char *out_file = NULL;
    int opt;
    int fd;
    int i;
    int one_shot = 0;
    pthread_t unlock_thread;
    struct stat st;

    while ((opt = getopt(argc, argv, "f:o:nh")) != -1) {
        switch (opt) {
        case 'f': fifo_name = optarg; break;
        case 'o': out_file = optarg; break;
        case 'n': one_shot = 1; break;
        default:
            fprintf(stderr, "usage: %s [-f FIFO] [-o FILE] [-n]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    fd = open(BUFFER_FILE, O_RDONLY);
    if (fd < 0) {
        perror(BUFFER_FILE);
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    buf_size = (size_t)st.st_size;
    buf = mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Writer-position sampler: its shadow starts as a copy of the ring
     * now, so the first diff sees everything written since startup. */
    shadow = malloc(buf_size);
    if (shadow) {
        pthread_t diff_tid;
        memcpy(shadow, (const void *)buf, buf_size);
        pthread_create(&diff_tid, NULL, diff_thread, NULL);
    }

    /* The fifo is opened before the start-point wait below, so a server
     * can attach immediately and just see no bytes until the gate passes. */
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
        /* The purger's read-open completes the fifo open dance and
         * keeps the write end from EPIPE; see fifo_purger_thread. */
        pthread_create(&unlock_thread, NULL, fifo_purger_thread,
                       (void *)fifo_name);
        /* NON-BLOCKING writer: a full fifo must make emit_nal DROP the
         * NAL (and stay glued to the writer), never block - blocking
         * here is what let the walk fall behind during idle periods
         * and served the next client a backlog sprint. */
        out_fd = open(fifo_name, O_WRONLY);
        if (out_fd < 0) {
            perror(fifo_name);
            return 1;
        }
        fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL, 0) | O_NONBLOCK);
        /* 256 KB fifo (kernel default is 64 KB): big enough for a
         * complete SPS+PPS+IDR chain (the join window the server's
         * startup drain keeps), small enough to keep the end-to-end
         * latency near one GOP. On this kernel the call only works
         * from the first reader's side; the server sets it there too,
         * so failure here is not fatal. */
        {
            int cur = fcntl(out_fd, F_GETPIPE_SZ);
            if (fcntl(out_fd, F_SETPIPE_SZ, 256 * 1024) != 0)
                fprintf(stderr,
                        "F_SETPIPE_SZ failed: %s (was %d, stays %d)\n",
                        strerror(errno), cur, fcntl(out_fd, F_GETPIPE_SZ));
            else
                fprintf(stderr, "F_SETPIPE_SZ ok: %d\n",
                        fcntl(out_fd, F_GETPIPE_SZ));
        }
    }

    /* Wait for a decodable start point: a complete SPS->PPS->IDR chain of
     * the target stream. The ring holds ~1.7 MB (a few seconds of
     * video), so once the app is writing, a chain appears within one GOP
     * interval; give it up to ~30 s. */
    stream_base = SCAN_START;
    for (i = 0; !find_idr_start(&stream_base); i++) {
        if (i == 0)
            fprintf(stderr, "waiting for SPS/PPS/IDR chain...");
        if (i >= 600) {
            fprintf(stderr, " none found in %s\n", BUFFER_FILE);
            fclose(out);
            munmap((void *)buf, buf_size);
            return 1;
        }
        usleep(50 * 1000);
    }
    fprintf(stderr, "IDR start at 0x%zX, target %ux%u, buffer %zu bytes\n",
            stream_base, target_w, target_h, buf_size);

    /* Same as the re-sync delay: with an empty fifo the initial walk
     * outruns the writer and reads its half-written slices. */
    usleep(1000 * 1000);

    if (one_shot) {
        /* dump one sweep of the target stream, ignoring the gate */
        drain_round(&stream_base, head_estimate(NULL), 1);
    } else {
        drain_forever();
    }

    fclose(out);
    munmap((void *)buf, buf_size);
    return 0;
}
