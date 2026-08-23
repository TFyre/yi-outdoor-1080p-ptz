/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// "liveMedia"
// Copyright (c) 1996-2023 Live Networks, Inc.  All rights reserved.
// A file source that is a plain byte stream (rather than frames)
// Implementation

#include "ByteStreamFifoSource.hh"
#include "presentationTime.hh"
#include "InputFile.hh"
#include "GroupsockHelper.hh"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

////////// ByteStreamFifoSource //////////

extern int debug;

ByteStreamFifoSource*
ByteStreamFifoSource::createNew(UsageEnvironment& env, char const* fileName,
                                unsigned preferredFrameSize,
                                unsigned playTimePerFrame) {
    int flags;
    FILE* fid = OpenInputFile(env, fileName);
    if (fid == NULL) return NULL;

    // Grow the fifo to 1 MB (kernel default 64 KB) so a whole GOP fits in
    // the drain window below. Done here, with only this reader attached:
    // F_SETPIPE_SZ fails with EEXIST once other handles are open (the
    // producer's identical call on its write end fails for this reason and
    // is ignored there).
    (void)fcntl(fileno(fid), F_SETPIPE_SZ, 1024 * 1024);
    if (debug & 4)
        fprintf(stderr, "fifo pipe size: %d\n",
                (int)fcntl(fileno(fid), F_GETPIPE_SZ));

    // Set non blocking
    if ((flags = fcntl(fileno(fid), F_GETFL, 0)) < 0) {
        fclose(fid);
        return NULL;
    };
    if (fcntl(fileno(fid), F_SETFL, flags | O_NONBLOCK) != 0) {
        fclose(fid);
        return NULL;
    };

    // Drain stale fifo content, but KEEP the tail from the last complete
    // SPS->PPS->IDR chain (fallback: last SPS). Blindly discarding
    // everything makes the first client join mid-GOP: the producer's
    // IDR-gated stream head gets thrown away whenever the server starts
    // after the producer has begun writing. The kept tail is served
    // before any further fifo reads, so joins decode from a keyframe.
    // The producer sizes the fifo at 1 MB (F_SETPIPE_SZ) so a whole GOP
    // fits in the drain window.
    ByteStreamFifoSource* newSource = NULL;
    {
#define DRAIN_CAP (2048*1024)
        unsigned char* drained = new unsigned char[DRAIN_CAP];
        unsigned total = 0;
        unsigned keep = 0, found = 0;
        unsigned chain_sps = 0;
        int attempt;

        /* Drain the fifo and keep the tail from the last complete
         * SPS->PPS->IDR chain. If no chain is in the window (e.g. the
         * DESCRIBE's dummy source drained the fifo moments ago), wait for
         * the producer's next GOP (~2.4 s) instead of giving the client a
         * mid-GOP join with a data gap. */
        for (attempt = 0; attempt < 16; attempt++) {
            while (total < DRAIN_CAP) {
                size_t n = fread(drained + total, 1, DRAIN_CAP - total, fid);
                if (n == 0) break;   /* fifo momentarily empty (EAGAIN) */
                total += (unsigned)n;
            }

            /* scan for the last chain (SEI/AUD allowed inside). The
             * producer emits 4-byte start codes (00 00 00 01); match those
             * so the kept tail begins with a full code — LIVE555's framer
             * only syncs on the 4-byte form. */
            int have_sps = 0;        /* 0 none, 1 SPS, 2 SPS+PPS */
            for (unsigned i = 0; i + 5 <= total; i++) {
                if (drained[i] != 0 || drained[i+1] != 0 ||
                    drained[i+2] != 0 || drained[i+3] != 1)
                    continue;
                unsigned t = drained[i+4] & 0x1F;
                switch (t) {
                case 7: chain_sps = i; have_sps = 1; keep = i; found = 1; break;
                case 8: if (have_sps == 1) have_sps = 2; break;
                case 5:
                    if (have_sps == 2) { keep = chain_sps; found = 2; have_sps = 0; }
                    break;
                case 6: case 9: break;
                default: have_sps = 0; break;
                }
            }
            if (found == 2) break;
            if (attempt < 15) {
                /* no complete chain yet; give the producer a moment */
                usleep(200000);
            }
        }

        newSource
            = new ByteStreamFifoSource(env, fid, preferredFrameSize, playTimePerFrame);
        if (total > keep) {
            newSource->fPendingData = new unsigned char[total - keep];
            memcpy(newSource->fPendingData, drained + keep, total - keep);
            newSource->fPendingLen = total - keep;
            newSource->fPendingOff = 0;
            if (debug & 4)
                fprintf(stderr, "fifo drain: kept %u bytes from 0x%x (%s), head:"
                        " %02x %02x %02x %02x %02x %02x\n",
                        total - keep, keep,
                        found == 2 ? "complete chain" :
                        found == 1 ? "SPS only" : "mid-GOP (no chain in window)",
                        newSource->fPendingData[0], newSource->fPendingData[1],
                        newSource->fPendingData[2], newSource->fPendingData[3],
                        newSource->fPendingData[4], newSource->fPendingData[5]);
        }
        delete[] drained;
    }

    // NOTE: the fd stays non-blocking on purpose; the constructor makes it
    // non-blocking for the background-handler path anyway, and the fifo
    // reader must never block (EAGAIN is handled in doReadFromFile).
    return newSource;
}

ByteStreamFifoSource*
ByteStreamFifoSource::createNew(UsageEnvironment& env, FILE* fid,
                                unsigned preferredFrameSize,
                                unsigned playTimePerFrame) {
    if (fid == NULL) return NULL;

    ByteStreamFifoSource* newSource = new ByteStreamFifoSource(env, fid, preferredFrameSize, playTimePerFrame);

    return newSource;
}

ByteStreamFifoSource::ByteStreamFifoSource(UsageEnvironment& env, FILE* fid,
                                           unsigned preferredFrameSize,
                                           unsigned playTimePerFrame)
    : FramedFileSource(env, fid), fFileSize(0), fPreferredFrameSize(preferredFrameSize),
      fPlayTimePerFrame(playTimePerFrame), fLastPlayTime(0),
      fHaveStartedReading(False), fLimitNumBytesToStream(False), fNumBytesToStream(0),
      fPendingData(NULL), fPendingLen(0), fPendingOff(0) {
#ifndef READ_FROM_FILES_SYNCHRONOUSLY
    makeSocketNonBlocking(fileno(fFid));
#endif
}

ByteStreamFifoSource::~ByteStreamFifoSource() {
    delete[] fPendingData;
    if (fFid == NULL) return;

#ifndef READ_FROM_FILES_SYNCHRONOUSLY
    envir().taskScheduler().disableBackgroundHandling(fileno(fFid));
#endif

    CloseInputFile(fFid);
}

void ByteStreamFifoSource::doGetNextFrame() {
    if (feof(fFid) || (fLimitNumBytesToStream && fNumBytesToStream == 0)) {
        handleClosure();
        return;
    }

#ifdef READ_FROM_FILES_SYNCHRONOUSLY
    doReadFromFile();
#else
    if (fPendingOff < fPendingLen) {
        // Kept drain tail still to serve: read it now instead of waiting
        // for fifo activity (the fifo may well be empty at this point).
        doReadFromFile();
        return;
    }
    if (!fHaveStartedReading) {
        // Await readable data from the file:
        envir().taskScheduler().setBackgroundHandling(fileno(fFid), SOCKET_READABLE,
               (TaskScheduler::BackgroundHandlerProc*)&fileReadableHandler, this);
        fHaveStartedReading = True;
    }
#endif
}

void ByteStreamFifoSource::doStopGettingFrames() {
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
#ifndef READ_FROM_FILES_SYNCHRONOUSLY
    envir().taskScheduler().disableBackgroundHandling(fileno(fFid));
    fHaveStartedReading = False;
#endif
}

void ByteStreamFifoSource::fileReadableHandler(ByteStreamFifoSource* source, int /*mask*/) {
    if (!source->isCurrentlyAwaitingData()) {
      source->doStopGettingFrames(); // we're not ready for the data yet
      return;
    }
    source->doReadFromFile();
}

void ByteStreamFifoSource::retryRead(ByteStreamFifoSource* source) {
    // Periodic fallback for the background handler: a missed readable
    // transition on the fifo leaves the server idle with a full fifo and
    // the producer blocked in pipe_wait (the stream stalls until another
    // client's drain shakes it loose). Poll every 100 ms while awaiting.
    source->nextTask() = NULL;
    if (source->isCurrentlyAwaitingData())
        source->doReadFromFile();
}

void ByteStreamFifoSource::doReadFromFile() {
    // Serve the kept drain tail first (see createNew): it ends exactly
    // where the fifo's current content begins, so the stream stays
    // contiguous across the transition.
    if (fPendingOff < fPendingLen) {
        fFrameSize = fPendingLen - fPendingOff;
        if (fFrameSize > fMaxSize) fFrameSize = fMaxSize;
        memcpy(fTo, fPendingData + fPendingOff, fFrameSize);
        fPendingOff += fFrameSize;
    } else {
        // Try to read as many bytes as will fit in the buffer provided (or "fPreferredFrameSize" if less)
        if (fLimitNumBytesToStream && fNumBytesToStream < (u_int64_t)fMaxSize) {
            fMaxSize = (unsigned)fNumBytesToStream;
        }
        if (fPreferredFrameSize > 0 && fPreferredFrameSize < fMaxSize) {
            fMaxSize = fPreferredFrameSize;
        }
#ifdef READ_FROM_FILES_SYNCHRONOUSLY
        fFrameSize = fread(fTo, 1, fMaxSize, fFid);
        if (fFrameSize == 0) {
            handleClosure();
            return;
        }
#else
        {
            /* The fifo is non-blocking: read() returns -1/EAGAIN when no
             * frame is buffered yet. Treat that as "wait for the
             * background handler" rather than converting it to a huge
             * unsigned frame size (which made the H264 parser read 4GB
             * and segfault). */
            ssize_t n = read(fileno(fFid), fTo, fMaxSize);
            if (n == 0) {
                handleClosure();
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* fifo momentarily empty: also arm the polling retry
                     * (see retryRead) so a missed handler transition
                     * cannot stall the stream */
                    nextTask() = envir().taskScheduler().scheduleDelayedTask(
                        100000, (TaskFunc*)&retryRead, this);
                    return;
                }
                handleClosure();
                return;
            }
            fFrameSize = (unsigned)n;
        }
#endif
    }
    fNumBytesToStream -= fFrameSize;

    // Set the 'presentation time':
    if (fPlayTimePerFrame > 0 && fPreferredFrameSize > 0) {
        if (fPresentationTime.tv_sec == 0 && fPresentationTime.tv_usec == 0) {
            // This is the first frame, so use the current time:
            gettimeofday(&fPresentationTime, NULL);
        } else {
#ifndef PRES_TIME_CLOCK
            // Increment by the play time of the previous data:
            unsigned uSeconds	= fPresentationTime.tv_usec + fLastPlayTime;
            fPresentationTime.tv_sec += uSeconds/1000000;
            fPresentationTime.tv_usec = uSeconds%1000000;
#else
            // Use system clock to set presentation time
            gettimeofday(&fPresentationTime, NULL);
#endif
        }

        // Remember the play time of this data:
        fLastPlayTime = (fPlayTimePerFrame*fFrameSize)/fPreferredFrameSize;
        fDurationInMicroseconds = fLastPlayTime;
    } else {
        // We don't know a specific play time duration for this data,
        // so just record the current time as being the 'presentation time':
        gettimeofday(&fPresentationTime, NULL);
        fDurationInMicroseconds = fPlayTimePerFrame;
    }
    if (debug & 4) fprintf(stderr, "h264 frame - fPresentationTime, sec = %ld, usec = %ld\n", fPresentationTime.tv_sec, fPresentationTime.tv_usec);

    // Inform the reader that he has data:
#ifdef READ_FROM_FILES_SYNCHRONOUSLY
    // To avoid possible infinite recursion, we need to return to the event loop to do this:
    nextTask() = envir().taskScheduler().scheduleDelayedTask(0,
                                (TaskFunc*)FramedSource::afterGetting, this);
#else
    // Because the file read was done from the event loop, we can call the
    // 'after getting' function directly, without risk of infinite recursion:
    FramedSource::afterGetting(this);
#endif
}
