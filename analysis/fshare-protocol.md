# The fshare shared-frame protocol

Reverse-engineered 2026-08-25 from `/home/app/tserver` (stock firmware
`5.0.00.00_202204281015`), then validated against live ring snapshots and the
running camera.

`tserver` is the ideal specimen: it is the only stock consumer that is **not**
UPX-packed, it is 18 KB, and it carries a complete private copy of the fshare
client code with the `fshare_open` / `fshare_create` / `fshare_write` log
strings still in `.rodata`. Everything below marked *verified-in-asm* is read
directly out of its instructions; addresses are `tserver` virtual addresses.

**Headline: the ring is not a byte stream that has to be re-synchronised.** It
is a sequence-numbered record log with a published tail, a published length, a
17-entry reader table, and a per-reader condition variable. A registered reader
never scans for start codes, never guesses the write head, and cannot tear —
it is handed one whole record at a time. Everything `fshare2fifo` currently
does (diff sampling, lap detection, head estimation, tear retries) exists only
to reconstruct information the header already publishes.

## Verification tags

| Tag | Meaning |
|---|---|
| **ASM** | Read directly from `tserver` instructions, cited inline |
| **DATA** | Confirmed by parsing real ring snapshots and/or the live header |
| **INF** | Inferred from strong but indirect evidence |
| **HYP** | Hypothesis, not established |

## 1. Objects

| Object | Kind | Purpose |
|---|---|---|
| `/fshare_frame_buf` | POSIX shm, **1786156 B (0x1B412C)** | header + record ring |
| `/fshare_write_lock` | named sem, init 1 | writer exclusion; held *collectively by the readers* |
| `/fshare_read_lock` | named sem, init 1 | mutex over the header and the slot table |
| `/fshare_read_notify_0` … `_16` | named sem, init 0 | per-slot wakeup, 17 of them |

**ASM** — `0x124e0` opens exactly this set: `sem_open("/fshare_read_lock", 2)`,
`shm_open("/fshare_frame_buf", 2, 0)`, `mmap(NULL, 1786156, 3, 1, fd, 0)`,
`sem_open("/fshare_write_lock", 2)`, then a 17-iteration loop
(`cmp r4, #17` at `0x125dc`) over `snprintf(buf, 23, "/fshare_read_notify_%d", i)`.
The mapping length is the literal `0x001b412c` at `0x12704` — **the size is
hardcoded in the client, not taken from `fstat`**.

Note the POSIX names have no `sem.` prefix; that prefix is what the kernel puts
on the tmpfs backing files in `/dev/shm`.

## 2. Header map

The mapping begins with a **300-byte (0x12C) header**, followed by the record
ring of **exactly 0x1B4000 = 1785856 bytes**. `300 + 0x1B4000 = 1786156` — the
whole mapping, exactly.

**ASM** — the data region base is `base + 300` (`add r8, r2, #300` at `0x12a08`,
`add r7, fp, #300` at `0x12c1c`, and the wrap fixup in the copy helper at
`0x12324`). Ring offsets wrap by `subgt rX, rX, #0x1b4000` once they exceed the
literal `0x001b3fff` (`0x12b74`, `0x12d30`).

| Off | Type | Field | Tag |
|---|---|---|---|
| 0x00 | u32 | **active reader count** — readers-writer lock state | ASM |
| 0x04 | u32 | **valid bytes** currently in the ring | ASM+DATA |
| 0x08 | u32 | unidentified; a slowly-varying byte magnitude (228720 / 236048 / 276961 observed) | — |
| 0x0C | u32 | **write head offset** = `(tail + valid) mod 0x1B4000` | DATA |
| 0x10 | u32 | **tail offset** — the oldest surviving record | ASM+DATA |
| 0x14 | u32 | **now timestamp** — equals the newest record's `+16` | ASM+DATA |
| 0x18 | u32 | **newest sequence number** — equals the newest record's `+4` | ASM+DATA |
| 0x1C–0x12B | — | **`reader_slot[17]`, stride 16** | ASM |
| 0x12C | — | record ring, 0x1B4000 bytes | ASM |

`0x0C` is *derived*, not independent: on four snapshots and the live header,
`(hdr[0x10] + hdr[0x04]) mod 0x1B4000 == hdr[0x0C]` exactly, every time. It is
the write head — the position the next record header will occupy.

`tserver` never reads `0x08`, so the disassembly cannot name it. It is stable
across minutes and then steps; it is **not** a ring offset (no valid record
header lives at `0x08`, at `head - 0x08`, or at `tail + 0x08`). The
`fshare.%s: size(%d) > size_max(%d)` and `try_adjust_buf` strings suggest a
capacity/reserve figure.

### Reader slot (16 bytes, `slot[k]` at `0x1C + 16*k`)

| Off | Type | Field | Tag |
|---|---|---|---|
| +0 | u32 | **pending** — nonzero means "there may be data for you" | ASM |
| +4 | u32 | **waiting** — reader is parked in `sem_wait(notify[k])` | ASM |
| +8 | u32 | **cursor** — sequence number of the last record consumed | ASM |
| +12 | u16 | **filter** — subscription mask; **0 = slot free** | ASM / INF |
| +14 | u16 | padding, always 0 | DATA |

**ASM** — `add r4, r4, #28` then `add r4, r4, r0, lsl #4` at `0x12b94`/`0x12b9c`
gives `&slot[idx]`; the field offsets come from `ldr r3,[r4]` (+0),
`str r6,[r4,#4]` (+4), `ldr r0,[r6,#8]` (+8), `ldrh r1,[r6,#12]` (+12).

`slot+12 == 0` meaning "free" is **INF**: it is the only field a reader sets on
registration that a writer could test, all 14 unused slots read as all-zero
live, and a slot that was occupied earlier today (`slot[6]`, filter `0x0400`)
is now fully zeroed — so something does clear slots on release.

## 3. Record framing

Every record is a **26-byte header followed by `len` payload bytes**; the next
record starts at `off + 26 + len`, wrapping at 0x1B4000.

**ASM** — `mov r2, #26` before each header copy (`0x127fc`, `0x12a00`,
`0x12cb4`), and the advance `add r2, r3, #26; add r4, r2, r4` at
`0x12a38`/`0x12a44` where `r3` is the u32 at header +0.

| Off | Type | Field | Tag |
|---|---|---|---|
| +0 | u32 | payload length | ASM+DATA |
| +4 | u32 | **sequence number**, +1 per record, ring-wide | ASM+DATA |
| +8 | u32 | opaque; delivered to the caller as `out+0` | ASM |
| +12 | u32 | opaque; delivered as `out+4` | ASM |
| +16 | u32 | **timestamp**, same clock as header `0x14` | ASM+DATA |
| +20 | u16 | **type** (see below) | ASM+DATA |
| +22 | u16 | opaque; delivered as `out+12` | ASM |
| +24 | u16 | opaque; delivered as `out+20` | ASM |

### Validation (DATA)

`analysis/verify_fshare_map.py` walks a snapshot from `hdr[0x10]` for exactly
`hdr[0x04]` bytes using this framing. On `ring_n.bin` and `ring_l.bin`:

```
ring_n.bin: entries=515  remaining_budget=0  seq_contiguous=True  ts_monotonic=True
            seq 455815..456329  (hdr 0x18=456329, match=True)
            last_ts=968818803   (hdr 0x14=968818803, delta=0)
            types: 0x0400x197 0x0100x192 0x0800x99 0x0422x6 0x0404x6 0x0401x6
                   0x0822x3 0x0804x3 0x0801x3
```

The walk consumes the advertised byte count to **exactly zero**, the sequence
numbers are contiguous with **no gaps**, and the final record's `seq` and `ts`
match `hdr[0x18]` and `hdr[0x14]` bit for bit. That is a complete framing
proof.

It also settles an old question: **there is no "raw Annex-B mode" and there are
no `00 00 01 c0` table entries.** The ring is record-framed 100% of the time.
The "modes" in `CLAUDE.md` were the blind walker mis-syncing. The earlier
hand-derived header was also shifted: what was read as `[2:6] counter` is
`+4 seq`, `[14:18] ts` is `+16`, `[18:20] type` is `+20`, and the header is 26
bytes, not 24 or 25.

### Type field and the subscription filter

The type u16 splits into a **class byte** (high) and a **subtype byte** (low):

| Class bit | Meaning |
|---|---|
| 0x0100 | audio (AAC) |
| 0x0200 | unidentified — no records of this class in any snapshot |
| 0x0400 | hi-res video (1920×1088) |
| 0x0800 | low-res video |

| Subtype | Meaning |
|---|---|
| 0x00 | plain frame |
| 0x01 | SPS #2 |
| 0x02 | SPS #1 |
| 0x04 | PPS |

Observed types are exactly `{0x0400, 0x0800, 0x0100}` ∪ `{0x0422, 0x0401,
0x0404}` (hi-res chain) ∪ `{0x0822, 0x0801, 0x0804}` (low-res chain).

The filter test lives at **`0x1247c`** and is applied to every candidate record:

```c
/* r0 = slot.cursor, r1 = slot.filter, r2 = &record_header */
static int entry_wanted(uint32_t cursor, uint16_t filter, const rec_hdr *h)
{
    if ((filter & 0x0F) && (filter & 0x0F) != (h->type & 0x0F))
        return 0;                                  /* 1248c..12494 */
    if (!(h->type & (filter & 0xFF00)))
        return 0;                                  /* 124a4..124ac */
    if (cursor == 0)          return 1;            /* 124b0: fresh reader */
    uint32_t want = (cursor == 0xFFFFFFFFu) ? 1 : cursor + 1;   /* 124bc */
    return (uint32_t)(h->seq - want) <= 0x7FFFFFFEu;            /* 124cc..124d8 */
}
```

So: the filter's low nibble optionally pins the subtype (0 = don't care), the
filter's high byte must AND-overlap the class bits, and the last clause is a
wrap-safe *"is this record at or after my cursor+1"* test. `cursor == 0` is the
sentinel for "I have consumed nothing; give me anything".

`tserver`'s own filter values (**ASM**, literals at `0x11664`, `0x11668`,
`0x11814`, and `mov fp, #2304`): `0x0802` → low-res SPS#1, `0x0804` → low-res
PPS, `0x0801` → low-res SPS#2, then `0x0900` (low-res + audio, any subtype) for
the streaming loop. It serves the sub-stream as MPEG-TS — its `.rodata` carries
`Content-type: video/mp2t`.

Live slot filters right now: `slot[0]=0x0200`, `slot[1]=0x0D00`
(audio+hi+lo — a recorder), `slot[2]=0x0900`. Slots 3–16 are all-zero.

### The stock readers are registered but idle (DATA)

`analysis/slotwatch.c` samples the slot table every 2 ms, read-only. Over 12 s
(5161 samples) while the writer was producing normally:

```
samples=5161  newest_seq=449963  tail=1236069  valid=1727759
slot  filter  cursor_delta  waiting_seen  pending_edges  state
   0  0x0200             0             0              0  occupied
   1  0x0d00             0             0              0  occupied
   2  0x0900             0             0              0  occupied
```

**Not one of the three registered readers advanced its cursor, parked, or saw a
`pending` edge.** They hold slots and consume nothing. That is the real
explanation for `semprobe` seeing zero posts on all 17 notify semaphores: no
stock reader was waiting, so there was nothing for the writer to post. It fits
the consumers' jobs — `mp4record` records on events, `oss` uploads on events,
`p2p_tnp` streams when the phone app connects; all three were idle.

Two consequences. The notify path only comes alive when a stock consumer is
actively working, so it cannot be observed on an idle camera. And a v2 reader
will, in practice, usually be the only *active* reader — which means it will
also usually be the process that takes `/fshare_write_lock` on every single
read via the first-in rule of §4. Keep those critical sections tight.

## 4. What the two locks actually guard

This is the part that matters most for not breaking the camera.

**`/fshare_read_lock` is a plain mutex** over the header and the slot table.
Every reader operation takes it, does a few loads/stores, and releases it.

**`/fshare_write_lock` is held by the readers, collectively**, in the classic
first-in-locks / last-out-unlocks pattern, using `hdr[0x00]` as the count:

```
0x12218  rd_lock():                      0x12270  rd_unlock():
  sem_wait(read_lock)                      sem_wait(read_lock)
  if (++hdr[0x00] == 1)                    if (--hdr[0x00] == 0)
      sem_wait(write_lock)                     sem_post(write_lock)
  sem_post(read_lock)                      sem_post(read_lock)
```

**ASM** — `ldr r3,[r2]; add r3,r3,#1; cmp r3,#1; str r3,[r2]; beq 1225c` then
`ldr r0,[r4,#4]; bl sem_wait` (`0x1222c`–`0x12260`); the mirror image with
`sub`/`cmp #0`/`sem_post` at `0x12284`–`0x1229c`.

**Consequence: while any reader is inside a critical section, `rmm` cannot
write.** These sections are microseconds long by design. A third-party reader
that takes `rd_lock` and then blocks — on a fifo write, on a socket, on a
`printf` to a slow console — stalls the entire camera pipeline. Never hold it
across anything that can block. Never hold it while copying a payload out.

## 5. The reader API

Reconstructed from `tserver`. Addresses are `tserver`'s; the same code is
compiled into `rmm` and (packed) into the other consumers.

| Addr | Reconstructed signature |
|---|---|
| `0x124e0` | `int fshare_open(void)` — shm_open + mmap + 19 sem_open |
| `0x12160` | `void fshare_close(void)` — munmap + sem_close ×19 |
| `0x12218` | `void rd_lock(void)` |
| `0x12270` | `void rd_unlock(void)` |
| `0x122c0` | `void *ring_copy(void *src, void *dst, size_t n)` — wrapping memcpy, returns advanced src |
| `0x1247c` | `int entry_wanted(cursor, filter, hdr)` |
| `0x12738` | `void fshare_set_filter(int slot, uint16_t filter)` |
| `0x12774` | `int fshare_read_next(int slot, …)` — **oldest** unconsumed match |
| `0x12970` | `int fshare_read_latest(int slot, …)` — **newest** match, drops the backlog |
| `0x12b78` | `int fshare_wait(int slot)` |
| `0x12be8` | `int fshare_register(int slot, uint32_t start_age)` |
| `0x12d34` | `fshare_read_next_blocking()` — `read_next` + `wait` retry loop |
| `0x12d88` | `fshare_read_latest_blocking()` — `read_latest` + `wait` retry loop |

Return convention (**ASM**, `0x12d74`/`0x12dc8` branch on `cmn r0,#1; blt`):
`-2` = nothing matched, go to sleep and retry; `-1` = hard error, propagate;
`>= 0` = a record was delivered.

### 5.1 Registration — `fshare_register(slot, start_age)` @ `0x12be8`

**There is no allocation and no negotiation. The slot index is a compile-time
constant baked into each consumer.** `tserver` passes `mov r0, #16` at
`0x1159c`. Nothing in the function searches for a free slot, and nothing
records a pid. Registration is: seed the cursor, then set `pending = 1`.

```c
void fshare_register(int k, uint32_t start_age)
{
    rd_lock();
    if (start_age == 0 || hdr->valid <= 0) {
        slot[k].cursor = 0;                    /* 12c70: consume from the tail */
    } else {
        rec_hdr h; ring_copy(DATA + hdr->tail, &h, 26);   /* oldest record */
        if (start_age >= hdr->now_ts - h.ts) {
            slot[k].cursor = hdr->newest_seq;  /* 12c4c: start live, skip backlog */
        } else {
            /* 12cc4..12d1c: walk forward from the tail, advancing the cursor,
               until a record with ts >= h.ts + start_age (records whose type
               has bit 0x0200 are skipped by the ts test) */
            uint32_t target = h.ts + start_age, off = hdr->tail;
            int left = hdr->valid;
            while (left > 0) {
                rec_hdr e; ring_copy(DATA + off, &e, 26);
                if (!(e.type & 0x0200) && (int32_t)(e.ts - target) >= 0) break;
                slot[k].cursor = e.seq;
                off = (off + 26 + e.len) % 0x1B4000;
                left -= 26 + e.len;
            }
        }
    }
    slot[k].pending = 1;                       /* 12c7c */
    rd_unlock();
}
```

`fshare_set_filter` (`0x12738`) is a separate call and is what actually makes
the slot live: `sem_wait(write_lock_handle); slot[k].filter = f;
sem_post(...)`. **ASM** — `add r3, r3, r6, lsl #4; strh r5, [r3, #40]` at
`0x1275c`, i.e. `base + 16*k + 40` = `slot[k] + 12`.

### 5.2 The wait — `fshare_wait(slot)` @ `0x12b78`

A textbook condition variable. The predicate is `slot[k].pending`; the
`waiting` flag is published *under the lock* before sleeping, so the writer can
see that a wakeup is required.

```c
int fshare_wait(int k)
{
    for (;;) {
        rd_lock();
        if (slot[k].pending != 0) {      /* 12bbc: predicate */
            slot[k].waiting = 0;         /* 12bcc */
            rd_unlock();
            return 0;
        }
        slot[k].waiting = 1;             /* 12ba8 */
        rd_unlock();                     /* 12bac — released BEFORE sleeping */
        sem_wait(notify[k]);             /* 12bb4 */
    }
}
```

### 5.3 The read — `0x12774` (next) and `0x12970` (latest)

Both start their scan at **`hdr[0x10]`, the ring tail — never at a remembered
byte offset** — and walk `hdr[0x04]` bytes forward, applying `entry_wanted` to
each record. `read_next` delivers the first match; `read_latest` remembers the
last match (`movne fp, r4` at `0x12a3c`) and delivers that one.

Because position is recovered from the published tail on every call and
identity is a sequence number, **a reader that falls behind cannot desync**. It
simply resumes at the oldest surviving record; the records it missed are gone
and it never knows or cares. There is nothing to tear and no lap to detect.

Both finish identically:

```c
slot[k].cursor = delivered.seq;                  /* 128e8 / 12af4 */
if (delivered.seq == hdr->newest_seq)            /* 128dc..128e4 */
    slot[k].pending = 0;                         /* 128f8: caught up */
rd_unlock();
return payload_bytes;
```

and on no-match they set `slot[k].pending = 0` and return `-2` (`0x12918`,
`0x12b40`), which sends the blocking wrapper into `fshare_wait`.

The payload copy uses the same `ring_copy` helper and happens **inside** the
critical section (`0x128d4`, `0x12ae0` are before the `rd_unlock` at `0x12904`
/ `0x12b10`) — so the stock readers do hold the write lock for the duration of
a frame memcpy. Short, but not zero.

### 5.4 Complete reader loop

```c
#define RING_SZ   1786156u
#define DATA_OFF  300u
#define DATA_SZ   0x1B4000u
#define NSLOT     17

/* --- one-time --- */
fd   = shm_open("/fshare_frame_buf", O_RDWR, 0);
base = mmap(NULL, RING_SZ, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);
rdlk = sem_open("/fshare_read_lock",  0);
wrlk = sem_open("/fshare_write_lock", 0);
for (i = 0; i < NSLOT; i++) {
    snprintf(nm, sizeof nm, "/fshare_read_notify_%d", i);
    notify[i] = sem_open(nm, 0);
}
hdr  = (struct fshare_hdr *)base;
slot = (struct fshare_slot *)((char *)base + 0x1C);

/* --- claim slot K (K must not collide with a stock consumer) --- */
rd_lock();
slot[K].cursor  = hdr->newest_seq;   /* start live; 0 would mean "from the tail" */
slot[K].pending = 1;
rd_unlock();
sem_wait(wrlk); slot[K].filter = 0x0400; sem_post(wrlk);   /* hi-res, any subtype */

/* --- steady state --- */
for (;;) {
    /* wait for work */
    for (;;) {
        rd_lock();
        if (slot[K].pending) { slot[K].waiting = 0; rd_unlock(); break; }
        slot[K].waiting = 1;
        rd_unlock();
        sem_wait(notify[K]);
    }

    /* drain everything currently matching, oldest first */
    for (;;) {
        uint32_t off, left, seq = 0, len = 0;
        int got = 0;
        rec_hdr h;

        rd_lock();
        off = hdr->tail; left = hdr->valid;
        while (left > 0) {
            ring_copy(off, &h, 26);
            if (entry_wanted(slot[K].cursor, slot[K].filter, &h)) {
                if (h.len <= sizeof payload) {
                    ring_copy(off + 26, payload, h.len);   /* keep this short */
                    len = h.len; seq = h.seq; got = 1;
                }
                break;
            }
            off   = (off + 26 + h.len) % DATA_SZ;
            left -= 26 + h.len;
        }
        if (got) {
            slot[K].cursor = seq;
            if (seq == hdr->newest_seq) slot[K].pending = 0;
        } else {
            slot[K].pending = 0;
        }
        rd_unlock();

        if (!got) break;              /* nothing left -> go back to the wait */
        emit(payload, len, &h);       /* fifo write etc. — OUTSIDE the lock */
    }
}

/* --- release --- */
rd_lock();
slot[K].filter = 0; slot[K].pending = 0;
slot[K].waiting = 0; slot[K].cursor = 0;
rd_unlock();
```

`ring_copy(off, dst, n)` is the wrapping read of `n` bytes at ring offset `off`
from `base + DATA_OFF`, splitting at `DATA_SZ`.

## 6. Consequences for `fshare2fifo` v2

- **No sampler, no shadow buffer, no tear check, no lap detection, no head
  estimation, no MAX_LAG.** All of it is replaced by `hdr[0x10]`, `hdr[0x04]`
  and a sequence number. The 256 KB shadow window that made the producer an
  OOM-killer target disappears entirely.
- **No start-code scanning and no stream demux.** `filter = 0x0400` delivers
  the 1920×1088 stream and nothing else; the two-stream interleave that
  corrupts P-frames is a non-problem. The chain arrives as typed records
  (`0x0422`/`0x0401`/`0x0404`) instead of being pattern-matched.
- **The OSD clock cannot jump backward.** Records are delivered in sequence
  order, and out-of-order delivery is structurally impossible.
- **CPU should collapse.** `fshare2fifo` is currently the single largest
  consumer on the box (31.5% in `top`, above `rmm`'s 26.3%, load average 6.0).
  A slot reader does one memcpy per frame and sleeps otherwise.
- **Backlog policy is a one-line choice**, not an algorithm: scan-first
  (`read_next`) to stream everything, or scan-last (`read_latest`) to always
  serve the newest and silently drop the backlog. `tserver` uses `read_latest`
  for its streaming loop — the stock code reaches the same conclusion
  `CLAUDE.md` reached the hard way.

### Slot choice — the one real hazard

Indices are hardcoded per consumer, so a squatter can collide. Known: `tserver`
uses **16**. Live right now, slots **0, 1, 2** are occupied (filters `0x0200`,
`0x0D00`, `0x0900`) and **3–16 are all-zero**; a `0x0400` registration on slot
6 existed earlier today and has since been released, so occupancy is dynamic.

Recommended: pick a high slot away from both ends (**10–14**), and at startup
refuse to claim it if `slot[K].filter != 0`. Re-check after claiming. Do not
use 0, 1, 2, or 16.

### Cross-libc hazard (INF, important)

The camera's app is uClibc; our binaries are static musl. The `sem_t` layouts
and the value encodings of the two libcs are **not** compatible, and these
semaphores live in shared memory. Evidence that this is not theoretical: the
`/dev/shm` backing words of the two locks we never waited on are clean
(`write_lock` and `read_lock` both read `1 0 0 0`), while **all 17 notify
semaphores, which `semprobe` did `sem_timedwait` on, now read `-1 0 0 0`**.
musl's `sem_timedwait` does `a_cas(sem->__val, 0, -1)` before blocking and does
not restore the word on timeout — so that `-1` is residue our own probe left in
the app's semaphores. Under a uClibc reader that word may read as an enormous
unsigned count.

No harm is currently observable — the stock readers' cursors are advancing
normally and no process is spinning — and `/dev/shm` is tmpfs, so a reboot
clears it. But before v2 goes live: confirm the two libcs agree on the on-disk
`sem_t`, and prefer a hand-rolled futex wait on the semaphore word over
trusting musl's wrappers. The safe fallback, if they do not agree, is to skip
`notify[K]` entirely and poll `slot[K].pending` under `rd_lock` on a short
timer — correctness does not depend on the semaphore, only latency does.

## 7. What stays unproven

1. **`hdr[0x08]`** — read by no code in `tserver`; not a ring offset; a
   slowly-varying byte magnitude. Probably a capacity/reserve figure related to
   `try_adjust_buf` / `size_max`.
2. **The writer's post predicate** — *which* notify semaphores `rmm` posts and
   when. The reader side proves `waiting` is published for the writer's
   benefit, which makes "post only to slots with `waiting == 1`" the natural
   reading, but that is **HYP** until read out of `rmm`. Whether the writer
   also applies the filter before posting, and whether it sets `pending` to 1
   or to a count, is likewise unproven. Note this cannot be settled by
   observation on an idle camera — see the slotwatch result above.
3. **`slot+12 == 0` as the free/occupied marker** — INF, not ASM. No stock code
   *observed so far* tests it; the inference rests on it being the only
   candidate field and on slots being observed to zero out on release.
4. **Overwrite policy** — what the writer does to a reader whose cursor falls
   off the tail. The reader side degrades gracefully by construction, but
   whether the writer actively bumps a slow cursor or drops the reader is
   unknown.
5. **Record header `+8`, `+12`, `+22`, `+24`** — faithfully delivered to the
   caller, never interpreted by the fshare layer. `+22`/`+24` are plausibly
   width/height or a frame index (**HYP**).
6. **Class `0x0200`** — a live reader subscribes to it (slot 0, and its cursor
   advances quickly) yet no record of that class appears in any snapshot.
   Unexplained.
7. **Timestamp units** for `+16` / `hdr[0x14]`, and therefore the units of
   `fshare_register`'s `start_age` argument.
8. **Slot ownership per process** — `mp4record`, `oss` and `p2p_tnp` map the
   ring but are UPX-packed, so their indices were not read out. `rmm` (10
   threads) may also register internal readers.

## 8. Reproducing this

```bash
# disassembly
arm-linux-musleabi-objdump -d analysis/tserver.bin > /tmp/tserver.dis

# framing validation against snapshots
python3 analysis/verify_fshare_map.py ring_n.bin ring_l.bin

# live header + slot table, read-only, no tools needed on the camera
ssh ... "dd if=/dev/shm/fshare_frame_buf bs=1 count=300 2>/dev/null | od -A d -t u4"
```

ARM A32 PC-relative reminder for checking the citations: for `ldr rX,[pc,#N]`
at address `A` the literal sits at `A+8+N`; for the `add rX,pc,rX` at `B` the
result is `B+8+literal`. The fshare client's globals in `tserver` are at
`0x240D4`: `+0` mapping base, `+4` write_lock, `+8` read_lock, `+0x0C..+0x4C`
`notify[17]`, `+0x50` mapping end.
