# The fshare shared-frame protocol

Reverse-engineered 2026-08-25 from `/home/app/tserver` (stock firmware
`5.0.00.00_202204281015`), then validated against live ring snapshots and the
running camera.

`tserver` is the ideal specimen for the reader side: the only stock consumer
that is **not** UPX-packed, 18 KB, carrying a complete private copy of the
fshare client with the `fshare_open` / `fshare_create` / `fshare_write` log
strings still in `.rodata`. The writer side (§6) comes from `/home/app/rmm`.
Bare addresses are `tserver`'s; `rmm` addresses are marked as such.

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
| **ASM** | Read directly from instructions, cited inline (`ASM(rmm)` = from the writer) |
| **DATA** | Confirmed by parsing real ring snapshots and/or the live header |

Every claim in §§1–7 carries one of those two. Anything that reaches neither
bar is not stated as fact — it is listed in §8.

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
| 0x08 | u32 | **max payload high-water mark**, class 0x0200 only; readers size their receive buffer from it | ASM(rmm) |
| 0x0C | u32 | **write head offset** = `(tail + valid) mod 0x1B4000` | ASM(rmm)+DATA |
| 0x10 | u32 | **tail offset** — the oldest surviving record | ASM+DATA |
| 0x14 | u32 | **now timestamp** — equals the newest record's `+16` | ASM+DATA |
| 0x18 | u32 | **newest sequence number** — equals the newest record's `+4` | ASM+DATA |
| 0x1C–0x12B | — | **`reader_slot[17]`, stride 16** | ASM |
| 0x12C | — | record ring, 0x1B4000 bytes | ASM |

`0x0C` is *derived*, not independent: on four snapshots and the live header,
`(hdr[0x10] + hdr[0x04]) mod 0x1B4000 == hdr[0x0C]` exactly, every time. It is
the write head — the position the next record header will occupy.

`tserver` never reads `0x08`; `rmm` does. It is a **monotonic maximum of the
payload length of class-0x0200 records** (`rmm` `0x5fe98`–`0x5feac`:
`ldr r0,[r4,#8]; ldr r1,[r5,#16]; cmp r0,r1; strlt r1,[r4,#8]`, reachable only
from the 0x0200 branch), and `rmm`'s reader-side `ensure_capacity` at `0x5f700`
sizes its receive buffer as `hdr[0x08] + need`. That matches the observed
behaviour exactly: a byte magnitude that only ever increases
(228720 → 236048 → 276961).

### `0x04`, `0x0C` and `0x10` are one quantity, not three

`head = tail + valid`, always. Verified on every capture:

```
ring_now2.bin  (0x0C-0x04) mod 0x1B4000 = 1350648   0x10 = 1350648   match=True
ring_n.bin                                1696257          1696257   match=True
ring_l.bin                                 935247           935247   match=True
ring_k.bin                                 996265           996265   match=True
```

That identity explains the three things that make `0x04` look mysterious when
you watch it move:

- **It advances in exact lockstep with `0x0C`.** While the tail is stationary,
  `Δhead = Δvalid` by definition. Same deltas, same audio pacing.
- **It sits in a band whose ceiling is 1.786M.** The ceiling *is* `0x1B4000` =
  1785856: `valid` is capped at the ring size, and the writer keeps the ring
  essentially full, so `0x04` oscillates just under the cap.
- **Its offset from `0x0C` is piecewise-constant.** That offset *is the tail*:
  `(0x0C − 0x04) mod 0x1B4000 == 0x10`. It steps only when the writer pops to
  make room — which is the same observation as "`0x10` freezes for seconds
  then leaps".

### The commit point, and why `0x0C` looks like a reservation frontier

`rmm`'s `fshare_write` publishes the header fields **before** copying the
record bytes, all inside one `write_lock` critical section:

```
5fd60  bl sem_wait          <- take /fshare_write_lock
5fdcc  str r2, [r4, #4]     <- 0x04 valid      published
5fdec  str r2, [r4, #4]         (pop path / fast path)
5fe10  str r2, [r4, #24]    <- 0x18 seq        published
5fe18  str r7, [r4, #12]    <- 0x0C head       published
5fe70  bl 5f678             <- ring_copy: the record bytes are written HERE
5fe7c  bl 5f678                 (extras)
5fe88  bl 5f678                 (payload)
5fefc  bl sem_post              (notify, per matching slot)
5ff18  bl sem_post          <- release /fshare_write_lock
```

So there are two different answers depending on how you look:

- **Read lock-free, `0x0C` is a reservation frontier.** It can lead the actual
  bytes by up to one whole record. In the current era the largest record is
  36138 B and the p99 is 5418 B, which brackets the measured "leads by 6–11 KB"
  exactly — that lead is one in-flight video record, not a systematic offset.
- **Read under `read_lock`, `0x0C` is an exact commit point.** A reader holding
  `read_lock` holds `write_lock` too (first-reader rule, §4), so the writer
  cannot be inside that window. Every byte behind `0x0C` is committed.

**There is no per-stream committed end.** One ring, one head, one tail; hi-res,
low-res and audio are interleaved as typed records and share all three
pointers. The hi-res "end" is just the newest record with `type & 0x0400`, and
the walk finds it by filtering, not by a separate pointer.

> **There is no counter at `0x24`.** Offset `0x24` is `0x1C + 8` — it is
> **`slot[0].cursor`**, inside the first reader slot. The long-standing note
> about "a second counter at 0x24 tracking the frame counter" was reading one
> reader's cursor. Likewise the "two reader cursors at 0x0C/0x10 moving
> together ~2 KB apart" were the write head and the tail; the real per-reader
> cursors are `slot[k]+8`.

### Reader slot (16 bytes, `slot[k]` at `0x1C + 16*k`)

| Off | Type | Field | Tag |
|---|---|---|---|
| +0 | u32 | **pending** — nonzero means "there may be data for you" | ASM |
| +4 | u32 | **waiting** — reader is parked in `sem_wait(notify[k])` | ASM |
| +8 | u32 | **cursor** — sequence number of the last record consumed | ASM |
| +12 | u16 | **filter** — subscription mask; **0 = slot free** | ASM |
| +14 | u16 | padding, always 0 | DATA |

**ASM** — `add r4, r4, #28` then `add r4, r4, r0, lsl #4` at `0x12b94`/`0x12b9c`
gives `&slot[idx]`; the field offsets come from `ldr r3,[r4]` (+0),
`str r6,[r4,#4]` (+4), `ldr r0,[r6,#8]` (+8), `ldrh r1,[r6,#12]` (+12).

`slot+12 == 0` means "free" — not because anything tests it explicitly, but
because the writer's only slot predicate is `entry_type & (filter & 0xFF00)`
(§6), which a zero filter can never satisfy. There is no separate
registered/unregistered bit anywhere in the structure.

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
| +8 | u32 | **magic** (`0x6a8c….`/`0x6a89….` family) | ASM(rmm) |
| +12 | u32 | **caller cookie** — `0x01c69010` on chain parts, 0 on plain frames | ASM(rmm) |
| +16 | u32 | **timestamp**, same clock as header `0x14` | ASM+DATA |
| +20 | u16 | **type** (see below); bit `0x20` = "payload has an extras prefix" | ASM+DATA |
| +22 | u16 | **chain-part sequence** — the caller bumps it between SPS/PPS/IDR | ASM(rmm) |
| +24 | u16 | caller-supplied; 0 in every capture | ASM(rmm) |

`+0` includes the extras prefix: `rmm` computes `len = payload + fp` where `fp`
is 6, 5 or 0 selected by a subtype byte, and sets `type |= 0x20` when `fp != 0`
(`0x5fe60: orr ip, ip, #32`). So the old "`0x0422` SPS#1 has an 8-byte prefix"
observation is the `0x20` flag, and `26 + len` is always the exact stride.

### Validation (DATA)

`tools/verify_fshare_map.py` walks a snapshot from `hdr[0x10]` for exactly
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

Re-validated on a capture taken in the era that a full-ring marker scan called
"raw Annex-B, zero record markers":

```
ring_now2.bin: entries=2734  remaining_budget=0  seq_contiguous=True
               seq 543593..546326  (hdr 0x18=546326, match=True)
               last_ts=982145806   (hdr 0x14=982145806, delta=0)
               types: 0x0100x1011 0x0400x779 0x0800x779
                      0x0822x28 0x0804x28 0x0801x28 0x0422x27 0x0404x27 0x0401x27
```

Still perfectly record-framed. So **there is no "raw Annex-B mode" and there
are no `00 00 01 c0` table entries** — the ring is record-framed 100% of the
time, and a scan that finds zero markers is looking at the wrong offset. The
earlier hand-derived header was shifted by two: what was read as `[2:6]
counter` is `+4 seq`, `[14:18] ts` is `+16`, `[18:20] type` is `+20`, and the
header is 26 bytes, not 24 or 25. A magic-scan keyed to `+6` finds nothing;
the magic is at `+8`.

The same two-byte shift shows up in the pacing measurements: the "dominant
196–197 B step at 7.1/s = 24-byte header + ~172-byte AAC" is this map's median
record of **197 B = 26-byte header + 171-byte AAC**. Same bytes, same rate.

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

`tools/slotwatch.c` samples the slot table every 2 ms, read-only. Over 12 s
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

## 6. The writer side (`rmm`)

Read out of `rmm.bin`; `fshare_create` @ `0x5f828`, `fshare_open` @ `0x5fabc`,
`fshare_write` @ `0x5fd14`. A binary-wide scan for the fshare global (`0x19B214`,
same layout as `tserver`'s `0x240D4`) found exactly 17 references — so this is
the only ring writer in `rmm`.

### The post predicate (ASM — resolves the central question)

`fshare_write` holds **only `/fshare_write_lock`** (`sem_wait` at `0x5fd60`,
`sem_post` at `0x5ff18`) and never touches `read_lock` or `hdr[0x00]`. After the
record is written it walks all 17 slots (`0x5feb4`–`0x5fecc`, stride 16, from
`base+0x1C` to `base+0x12C`, with the notify array in lockstep):

```c
for (int i = 0; i < 17; i++) {
    uint16_t filter = slot[i].filter;
    if (!(entry_type & (filter & 0xFF00)))   /* 5fed8 bic / 5fedc tst */
        continue;
    slot[i].pending = 1;                     /* 5fee8 str — a store, not an increment */
    if (slot[i].waiting != 0)                /* 5fee4 ldr / 5feec cmp / 5fef0 beq */
        sem_post(notify[i]);                 /* 5fefc — the only post */
}
```

Three things fall out:

- **The writer posts only to slots whose reader is parked.** That is the
  complete explanation for `semprobe` seeing zero posts, and it composes with
  the slotwatch result: no stock reader was even consuming, let alone parked.
- **`filter == 0` is the free marker after all** — not via an explicit test, but
  because a zero filter can never satisfy the AND. There is no separate
  registered/unregistered bit anywhere.
- **The writer applies only the class AND.** The low-nibble subtype test and the
  cursor freshness test exist solely in the reader (`0x5f7c4` in `rmm`,
  `0x1247c` in `tserver`). So `pending` is an over-approximation: it means
  "something in your class arrived", not "something you want". A reader must
  still scan and may legitimately find nothing.

The handshake is race-free: the writer sets `pending` and reads `waiting` under
`write_lock`, while the reader checks `pending` under `read_lock` — which, as
first reader, *is* `write_lock`. The reader publishes `waiting = 1` before
releasing, so any write that lands after the check necessarily observes it. No
lost wakeups.

### Create, and what is never initialised (ASM)

`fshare_create` is `shm_open("/fshare_frame_buf", O_CREAT|O_RDWR, 0644)` +
`ftruncate(fd, 0x1B412C)` + `mmap` + six `sem_open`s, and **stores nothing into
the mapping** — the header and all 17 slots start as ftruncate's zeros. The two
locks are created with value **1**, the 17 notify semaphores with value **0**
(`0x5f900: mov r3, sl` with `sl = 0`).

That is what makes the contamination argument below conclusive rather than
suggestive: a notify semaphore's word is `0` at creation and `-1` only while a
reader is parked — yet **all 17 read `-1`, including the 14 slots whose filter
is 0 and which therefore no reader can ever park on.**

### Overwrite policy: the writer ignores slow readers entirely (ASM)

When `valid + entrysize > 0x1B4000` the writer pops records off the tail
(`0x5fd9c`–`0x5fde0`), and per pop it updates **only** `hdr[0x04] -= popped` and
`hdr[0x10] = new tail`. No cursor is bumped, no slot is zeroed, there is no
"reader too slow" path, and no reader state is even read. A stale cursor is
simply left stale, and the reader's own freshness test is what sorts it out on
its next scan. Records larger than `0x1B4000 - 26` are rejected before the lock
with the `size(%d) > size_max(%d)` error.

### Per-write header maintenance (ASM)

| Field | Update |
|---|---|
| 0x04 | `+= 26 + len`, after the pop loop guarantees it fits (`0x5fdec`) |
| 0x0C | `head = (head + 26 + len) mod 0x1B4000` (`0x5fdf0`–`0x5fe18`) |
| 0x10 | written **only** in the pop path (`0x5fddc`) |
| 0x14 | monotonic max of `ts`, non-0x0200 records only (`0x5ff2c`–`0x5ff4c`) |
| 0x18 | `seq += 1`, skipping 0 on wrap (`0x5fe00`–`0x5fe14`) — every write |
| 0x08 | monotonic max payload length, class 0x0200 only (`0x5fe98`) |
| slot | `slot[i].pending = 1` on class match; `+4`/`+8`/`+12` never written |

A decimal/hex trap to watch for when re-reading this: objdump prints load
offsets in decimal, so the caught-up test's `ldr r2, [r3, #24]` is offset
**0x18**, not 0x24. `verify_fshare_map.py` confirms it from the other side —
the last record's `seq` equals `hdr[0x18]` exactly on every snapshot — and
`0x24` is `slot[0].cursor`.

## 7. Consequences for `fshare2fifo` v2

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
`mp4record`, `oss` and `p2p_tnp` are UPX-packed and their indices are unread —
this is the one unresolved item that can actually bite v2.

Recommended: pick a high slot away from both ends (**10–14**); under `rd_lock`,
refuse to claim it if `slot[K].filter != 0`, and re-check after claiming. A
collision is not silently benign — two readers sharing a slot would fight over
one cursor and each would see the other's records vanish.

Collateral damage is bounded, though: the writer's only per-slot action is
`pending = 1` plus a conditional post, and it never reads a cursor. So a
mis-set slot cannot corrupt the ring or wedge `rmm` — it can only starve or
confuse the reader that owns the slot.

### The notify semaphores are currently BROKEN, and we broke them

This is not a latent hazard. It is live damage, and it is ours.

**uClibc's `sem_t` layout**, read out of the camera's own
`/lib/libpthread.so.0` (it exports symbols):

| Word | Field | Proof |
|---|---|---|
| 0 | `value` | `__new_sem_init+0x60`: `str r2, [r0]` |
| 1 | `private` — 128 when process-private, 0 when shared | `stmib r0, {r1, r3}`, `moveq r1, #128` |
| 2 | `nwaiters` | `__new_sem_wait+0xb8`: `ldr r3, [r4, #8]` / `add r1, r3, #1` |
| 3 | unused | — |

`__new_sem_getvalue` returns word 0 **raw and unclamped** (`ldr r2, [r0]; str
r2, [r1]`). And the decisive instruction, `__new_sem_wait+0x78`:

```
5944:  ldr r3, [r4]        ; value
5948:  cmp r3, #0
594c:  beq 59d8            ; ONLY value == 0 goes to the futex-wait path
5950:  sub r1, r3, #1      ; otherwise decrement and return success
```

**uClibc blocks only when the value is exactly zero.** Any other value —
including `0xFFFFFFFF` — makes `sem_wait` return immediately.

`rmm` creates all 17 notify semaphores with value **0**. They now read:

```
notify_0 : 4294967295          (0xFFFFFFFF, untouched)
notify_1 : 4197658095 -> 4196341853 over 5 s   (-1,316,242 = ~263,000/s)
notify_2 : 4292755546          (static)
notify_3 : 4294967295          (0xFFFFFFFF — a FREE slot, filter == 0)
write_lock: 1     read_lock: 0
```

`notify_3` is the proof of authorship: `filter == 0`, so by §6 no reader can
ever park on it and no `sem_post` can ever target it. Nothing in the app has
touched that word since creation — yet it reads `0xFFFFFFFF` instead of `0`.
That is exactly what musl's `sem_timedwait` writes (`a_cas(sem->__val, 0, -1)`
before blocking, not restored on timeout), and `semprobe` timed-waited all 17.

The consequence is `notify_1`: **~263,000 decrements per second.** Slot 1's
reader (filter `0x0D00`) calls `sem_wait`, never blocks, loops in
`fshare_wait`, and burns CPU — taking `read_lock` (and therefore `write_lock`)
on every iteration. `mp4record` is in state `R` at 16% CPU on a box that is at
load 6.3 with 0% idle. It should be asleep.

Two mitigations, in order of preference:

1. **Reboot.** `/dev/shm` is tmpfs; the semaphores are recreated with value 0.
   Clean, and the user reboots by hand anyway.
2. **Wait it out.** At 263k/s the value drains from 4.2 G to 0 in roughly four
   hours, at which point the semaphore self-heals. Not recommended.

Do not "fix" it by storing 0 into the word: a reader is concurrently
`ldrex`/`strex`-ing it, and it is app state.

For v2: do not call musl's `sem_*` on these. Either implement `sem_wait`
against the uClibc layout directly (decrement word 0 unless zero; on zero,
`FUTEX_WAIT` on word 0 with `nwaiters` bumped), or — much safer — **skip the
notify semaphore entirely and poll `slot[K].pending` on a short timer.**
Correctness never depends on the semaphore; only wakeup latency does.

## 8. What stays unproven

The writer disassembly closed the big ones — the post predicate, `hdr[0x08]`,
the overwrite policy, and most of the record header. What is left:

1. **Class `0x0200`.** `rmm` clearly writes such records — they are the only
   thing that advances `hdr[0x08]`, and `hdr[0x08]` does advance — and
   `slot[0]` subscribes to exactly that class. Yet **no record of class 0x0200
   appears in any snapshot.** They are presumably rare and large (`hdr[0x08]`
   is now 276961 bytes, larger than any video record) — a stills/JPEG or
   event-blob channel would fit. Not captured, not identified.
2. **Timestamp units** for `+16` / `hdr[0x14]`, and therefore the units of
   `fshare_register`'s `start_age` argument.
3. **Record header `+24`** — caller-supplied, 0 in every capture, purpose
   unknown. (`+8` magic, `+12` cookie and `+22` chain-sequence are now read.)
4. **Who releases a slot.** Slots are observed to go from occupied to all-zero
   (`slot[6]`, filter `0x0400`, did so today), but neither `tserver` nor
   `rmm`'s writer contains the code that clears one. Presumably a
   `fshare_unregister` in the reader library that `tserver` does not call.
5. **Why `notify_2` is static while `notify_1` drains at 263k/s.** Both slots
   are occupied and both cursors advance. Slot 2's reader is presumably parked
   in something other than `fshare_wait`, or polls `pending` without ever
   calling `sem_wait` (as `rmm`'s own internal readers do).
6. **Slot ownership per process.** `mp4record`, `oss` and `p2p_tnp` map the
   ring but are UPX-packed, so their hardcoded indices were not read out.
   `rmm` also has two internal readers of its own (`0x5fff4`, `0x601f0`) which
   poll `pending` and never park. This is the one open item that directly
   affects v2 — see the slot-choice note.
7. **`fshare_open`'s callers in `rmm`** — no static `bl` reaches `0x5fabc`, so
   it is presumably dispatched through a function-pointer table.

## 9. Reproducing this

```bash
# disassembly (tserver = the reader, rmm = the writer)
arm-linux-musleabi-objdump -d analysis/tserver.bin > /tmp/tserver.dis
arm-linux-musleabi-objdump -d analysis/rmm.bin     > /tmp/rmm.dis

# framing validation against snapshots
python3 tools/verify_fshare_map.py ring_n.bin ring_l.bin

# live header + slot table, read-only, no tools needed on the camera
ssh ... "dd if=/dev/shm/fshare_frame_buf bs=1 count=300 2>/dev/null | od -A d -t u4"

# live slot activity, read-only (build with tools/build-armv6.sh's toolchain)
arm-linux-musleabi-gcc -O2 -static -no-pie -march=armv6 -mfloat-abi=soft \
    -o slotwatch tools/slotwatch.c
```

ARM A32 PC-relative reminder for checking the citations: for `ldr rX,[pc,#N]`
at address `A` the literal sits at `A+8+N`; for the `add rX,pc,rX` at `B` the
result is `B+8+literal`. The fshare client's globals in `tserver` are at
`0x240D4`: `+0` mapping base, `+4` write_lock, `+8` read_lock, `+0x0C..+0x4C`
`notify[17]`, `+0x50` mapping end.
