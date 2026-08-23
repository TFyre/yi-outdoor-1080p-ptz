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
 *   - the two streams' P-slices INTERLEAVE and share the 9a 00 shape;
 *     only the pic_order_cnt byte differs (0x90 vs 0x91 in raw mode).
 *     It is learned from the first slice after the join chain and then
 *     enforced (stream_q) - mixing both streams corrupts every P frame,
 *     filtering them apart decodes with zero errors
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
 * The ring is small (1.7 MB) and the writer laps it in ~4 s, so there is
 * no positional re-gate: while blocked on a full fifo the writer may
 * overwrite our position, and that is detected by watching the frame
 * counter across each blocking write (a block of ~1.2 s of ticks means
 * the bytes ahead were overwritten) and re-syncing to a fresh chain.
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
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define FRAME_COUNTER_OFFSET 0x18
/* The writer ticks the frame counter ~43/s. A fifo write that blocks for
 * this many ticks (>= ~1.2 s) means the app kept writing while we were
 * stuck and our read position has likely been overwritten. */
#define LAP_TICKS 50
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
/* Slice discriminator: the ring carries two interleaved streams whose
 * slices share the same 9a 00 header shape; only the pic_order_cnt byte
 * differs (0x90 for the target stream in raw mode, 0x91 for the other
 * one). Learned from the first slice after the join chain, then enforced.
 * 0 = not learned yet. */
static uint8_t stream_q;

/* Keep the fifo open for writing without blocking forever when no reader
 * has appeared yet: a helper thread opens it read-only to unblock us, then
 * parks with the fd open (a fifo writer with no reader gets EPIPE).
 *
 * NOTE: it must never read() from the fifo - an early version did and
 * silently stole the first 1024 bytes of the stream (the SPS+PPS+IDR
 * head), which made every client join mid-GOP. */
static void *unlock_fifo_thread(void *arg)
{
    const char *fifo_name = arg;
    int fd = open(fifo_name, O_RDONLY);
    if (fd >= 0) {
        while (1)
            pause();               /* hold the fd open; never consume data */
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
        if (stream_q == 0) {
            if (hd[6] < 0x80)
                return 0;             /* implausible; keep waiting */
            stream_q = hd[6];
            if (f2f_trace())
                fprintf(stderr, "stream slice byte = 0x%02x\n", stream_q);
        } else if (hd[6] != stream_q) {
            return 0;                 /* the other interleaved stream */
        }
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
 * header queue slots when no entry is present (record-mode rings). */
static uint32_t head_estimate(void)
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
    if (best_ctr)
        return best_pos;

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
        return h;
    }
}

/* Distance from pos to the (cached) head position. */
static size_t dist_to_head_cached(size_t pos, uint32_t head)
{
    uint32_t h = head % (uint32_t)buf_size;
    if (h >= pos)
        return h - pos;
    return (size_t)h + buf_size - pos;
}

/* Write all n bytes to fd, looping over partial writes.
 * Returns 0 on success, -1 if the fd would block (fifo full), -2 on a
 * hard error. */
static int write_all_fd(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return -2;
        }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Write n bytes, watching the frame counter across the call: if the
 * write blocked long enough for the writer to overwrite our read
 * position, report -3 so the caller re-syncs to a fresh chain. */
static int write_all_fd_lap(int fd, const void *p, size_t n)
{
    uint32_t c0 = frame_counter();
    int r = write_all_fd(fd, p, n);
    if (r != 0)
        return r;
    if (frame_counter() - c0 >= LAP_TICKS)
        return -3;
    return 0;
}

/* Emit one NAL (ring positions s..e) as 4-byte start code + body,
 * handling the ring wrap. Returns 0, -1 (would block), -2 (error), or
 * -3 (lapped while blocked). */
static int emit_nal(size_t s, size_t e)
{
    static const unsigned char start4[4] = {0x00, 0x00, 0x00, 0x01};

    if (out_fd >= 0) {
        int r = write_all_fd_lap(out_fd, start4, sizeof(start4));
        if (r != 0)
            return r;
        if (e > s) {
            r = write_all_fd_lap(out_fd, (const void *)(buf + s + 3), e - s - 3);
            if (r != 0)
                return r;
        } else {                  /* NAL spans the ring wrap */
            r = write_all_fd_lap(out_fd, (const void *)(buf + s + 3),
                                 buf_size - s - 3);
            if (r != 0)
                return r;
            if (e > 0) {
                r = write_all_fd_lap(out_fd, (const void *)buf, e);
                if (r != 0)
                    return r;
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
    uint32_t head = head_estimate();  /* once per call: the scan is costly */

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
                    /* slices break the chain but are never emitted here:
                     * the stream discriminator must be learned only from
                     * the walk that follows the chosen chain */
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
    /* The target must be a real high-res stream: if the dims scan missed
     * every high-res SPS (mid-lap overwrite), locking onto the low-res
     * stream (640x368) would switch the client's resolution - retry
     * instead. */
    if (target_w < 1280)
        return 0;
    if (!found_chain)
        return 0;
    *out = best;
    stream_q = 0;          /* re-learn the slice discriminator after the join */
    return 1;
}

/* Forward NAL units of the target stream while the producer keeps bumping
 * the frame counter. Called repeatedly; pos carries the read position
 * across calls. NAL start codes are written as 4-byte. With "force", the
 * counter catch-up check is skipped (one-shot dumps of static snapshots).
 * Returns 0, 1 (the writer lapped us while we were blocked: caller must
 * re-sync to a fresh chain), or 2 (hard error). */
static int drain_round(size_t *pos, uint32_t *last, int force)
{
    int i;

    for (i = 0; i < 256; i++) {
        size_t s = *pos, e;

        if (!next_sc(&s, 0))          /* nothing in the ring */
            return 0;
        e = s + 4;
        if (!next_sc(&e, 0) || e == s) /* no complete NAL yet */
            return 0;
        if (want_nal(s, e)) {
            int r = emit_nal(s, e);
            if (r != 0)
                return r == -3 ? 1 : r;
        }
        *pos = e;
        if (force)
            continue;
        if (frame_counter() == *last) /* caught up with the writer */
            return 0;
        *last = frame_counter();
    }
    return 0;
}

static void drain_forever(void)
{
    size_t pos = stream_base;
    uint32_t last = frame_counter();

    while (1) {
        uint32_t c = frame_counter();
        int r;

        if (c == last) {
            usleep(POLL_MS * 1000);
            continue;
        }
        last = c;
        r = drain_round(&pos, &last, 0);
        if (r == 1) {
            /* We stalled on the fifo long enough for the writer to lap
             * our read position (the ring only holds a few seconds of
             * video) - jump to the newest chain. */
            if (find_idr_start(&pos)) {
                last = frame_counter();
                fprintf(stderr, "re-sync: IDR start at 0x%zX\n", pos);
            } else {
                usleep(200 * 1000);
            }
        } else if (r == 2) {
            perror("write");
            return;
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
        /* unblock our writer-open in case no reader has connected yet */
        pthread_create(&unlock_thread, NULL, unlock_fifo_thread,
                       (void *)fifo_name);
        /* Blocking writer: the fifo paces us to the reader (a client
         * playing at real-time speed). While we block, the ring writer
         * laps our read position; the frame-counter watch in
         * write_all_fd_lap detects that once we unblock and re-syncs to
         * a fresh chain. The unlock thread's read-open keeps this open
         * from blocking forever when no real reader exists. */
        out_fd = open(fifo_name, O_WRONLY);
        if (out_fd < 0) {
            perror(fifo_name);
            return 1;
        }
        /* 1 MB fifo (kernel default is 64 KB) so a whole GOP fits in the
         * buffered window the server's startup drain keeps to give new
         * clients a decodable join point. On this kernel the call only
         * works from the first reader's side; the server sets it there
         * too, so failure here is not fatal. */
        {
            int cur = fcntl(out_fd, F_GETPIPE_SZ);
            if (fcntl(out_fd, F_SETPIPE_SZ, 1024 * 1024) != 0)
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

    if (one_shot) {
        /* dump one sweep of the target stream, ignoring the counter */
        uint32_t last = frame_counter() - 1;
        drain_round(&stream_base, &last, 1);
    } else {
        drain_forever();
    }

    fclose(out);
    munmap((void *)buf, buf_size);
    return 0;
}
