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
// A 'ServerMediaSubsession' object that creates new, unicast, "RTPSink"s
// on demand, from a H264 video fifo.
// Implementation

#include "H264VideoFifoServerMediaSubsession.hh"
#include "H264VideoRTPSink.hh"
#include "ByteStreamFileSource.hh"
#include "H264VideoStreamFramer.hh"
#include "H264VideoStreamFramer.hh"
#include "ByteStreamFifoSource.hh"
#include "FramedFilter.hh"
#include <sys/time.h>

// Per-client delivery pacing: a filter that holds each complete frame
// until its pace-clock slot (~15 fps wall time) instead of delivering
// "as fast as the source can supply". An unpaced server lets a greedy
// client buffer ahead - ffplay's packet queue held ~165 KB (the
// measured ~4 s of latency at the static bitrate) - and the client's
// queue-cap pauses flood the producer's fifo (whole-slice drops under
// motion). Pacing keeps the client's queue at a frame or two and the
// fifo near-empty; the producer's 256 KB fifo absorbs the per-frame
// jitter while a frame is held. The pace clock resyncs when the fifo
// has nothing to deliver, so a stalled stream recovers to live instead
// of sprinting its backlog.
class PacedFilter: public FramedFilter {
public:
    static PacedFilter* createNew(UsageEnvironment& env, FramedSource* inputSource) {
        return new PacedFilter(env, inputSource);
    }

protected:
    PacedFilter(UsageEnvironment& env, FramedSource* inputSource)
        : FramedFilter(env, inputSource), fPacingTask(NULL), fIsStopped(False) {
        fPacingTime.tv_sec = 0;
        fPacingTime.tv_usec = 0;
    }
    virtual ~PacedFilter() {
        if (fPacingTask != NULL)
            envir().taskScheduler().unscheduleDelayedTask(fPacingTask);
    }
    virtual void doGetNextFrame() {
        if (!fIsStopped) {
            fInputSource->getNextFrame(fTo, fMaxSize,
                                       afterGettingFrame, this,
                                       onSourceClosure, this);
        }
    }
    static void afterGettingFrame(void* clientData, unsigned frameSize,
                                  unsigned numTruncatedBytes,
                                  struct timeval presentationTime,
                                  unsigned durationInMicroseconds) {
        ((PacedFilter*)clientData)->afterGettingFrame1(frameSize,
                                                       numTruncatedBytes,
                                                       presentationTime,
                                                       durationInMicroseconds);
    }
    void afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes,
                            struct timeval presentationTime,
                            unsigned durationInMicroseconds) {
        if (fIsStopped) return;   /* teardown raced a delivery */

        fFrameSize = frameSize;
        fNumTruncatedBytes = numTruncatedBytes;
        fPresentationTime = presentationTime;
        fDurationInMicroseconds = durationInMicroseconds;

        struct timeval now;
        gettimeofday(&now, NULL);
        if (fPacingTime.tv_sec == 0 && fPacingTime.tv_usec == 0) {
            fPacingTime = now;    /* first frame: deliver immediately */
        } else {
            fPacingTime.tv_usec += 66667;   /* 1000000 / 15 */
            if (fPacingTime.tv_usec >= 1000000) {
                fPacingTime.tv_sec += 1;
                fPacingTime.tv_usec -= 1000000;
            }
            long delay = (fPacingTime.tv_sec - now.tv_sec) * 1000000L
                       + (fPacingTime.tv_usec - now.tv_usec);
            if (delay > 0) {
                /* hold the frame: deliver downstream on the pace clock */
                fPacingTask = envir().taskScheduler().scheduleDelayedTask(
                    delay, (TaskFunc*)pacingTimeout, this);
                return;
            }
            fPacingTime = now;    /* fell behind: resync */
        }
        afterGetting(this);       /* deliver downstream */
    }
    static void pacingTimeout(PacedFilter* filter) {
        filter->pacingTimeout1();
    }
    void pacingTimeout1() {
        fPacingTask = NULL;
        if (!fIsStopped)
            afterGetting(this);   /* deliver the held frame downstream */
    }
    static void onSourceClosure(void* clientData) {
        ((PacedFilter*)clientData)->handleClosure();
    }
    virtual void doStopGettingFrames() {
        fIsStopped = True;
        if (fPacingTask != NULL) {
            envir().taskScheduler().unscheduleDelayedTask(fPacingTask);
            fPacingTask = NULL;
        }
        FramedFilter::doStopGettingFrames();
    }

private:
    struct timeval fPacingTime;   /* pace clock: next delivery slot */
    TaskToken fPacingTask;        /* delayed delivery of the held frame */
    Boolean fIsStopped;
};

H264VideoFifoServerMediaSubsession*
H264VideoFifoServerMediaSubsession::createNew(UsageEnvironment& env,
                                              char const* fileName,
                                              Boolean reuseFirstSource) {
    return new H264VideoFifoServerMediaSubsession(env, fileName, reuseFirstSource);
}

H264VideoFifoServerMediaSubsession::H264VideoFifoServerMediaSubsession(UsageEnvironment& env,
                                                                       char const* fileName, Boolean reuseFirstSource)
    : FileServerMediaSubsession(env, fileName, reuseFirstSource),
      fAuxSDPLine(NULL), fDoneFlag(0), fDummyRTPSink(NULL) {
}

H264VideoFifoServerMediaSubsession::~H264VideoFifoServerMediaSubsession() {
    delete[] fAuxSDPLine;
}

static void afterPlayingDummy(void* clientData) {
    H264VideoFifoServerMediaSubsession* subsess = (H264VideoFifoServerMediaSubsession*)clientData;
    subsess->afterPlayingDummy1();
}

void H264VideoFifoServerMediaSubsession::afterPlayingDummy1() {
    // Unschedule any pending 'checking' task:
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
    // Signal the event loop that we're done:
    setDoneFlag();
}

static void checkForAuxSDPLine(void* clientData) {
    H264VideoFifoServerMediaSubsession* subsess = (H264VideoFifoServerMediaSubsession*)clientData;
    subsess->checkForAuxSDPLine1();
}

void H264VideoFifoServerMediaSubsession::checkForAuxSDPLine1() {
    nextTask() = NULL;

    char const* dasl;
    if (fAuxSDPLine != NULL) {
        // Signal the event loop that we're done:
        setDoneFlag();
    } else if (fDummyRTPSink != NULL && (dasl = fDummyRTPSink->auxSDPLine()) != NULL) {
        fAuxSDPLine = strDup(dasl);
        fDummyRTPSink = NULL;

        // Signal the event loop that we're done:
        setDoneFlag();
    } else if (!fDoneFlag) {
        // try again after a brief delay:
        int uSecsToDelay = 100000; // 100 ms
        nextTask() = envir().taskScheduler().scheduleDelayedTask(uSecsToDelay,
                (TaskFunc*)checkForAuxSDPLine, this);
  }
}

char const* H264VideoFifoServerMediaSubsession::getAuxSDPLine(RTPSink* rtpSink, FramedSource* inputSource) {
    if (fAuxSDPLine != NULL) return fAuxSDPLine; // it's already been set up (for a previous client)

    if (fDummyRTPSink == NULL) { // we're not already setting it up for another, concurrent stream
        // Note: For H264 video files, the 'config' information ("profile-level-id" and "sprop-parameter-sets") isn't known
        // until we start reading the file.  This means that "rtpSink"s "auxSDPLine()" will be NULL initially,
        // and we need to start reading data from our file until this changes.
        fDummyRTPSink = rtpSink;

        // Start reading the file:
        fDummyRTPSink->startPlaying(*inputSource, afterPlayingDummy, this);

        // Check whether the sink's 'auxSDPLine()' is ready:
        checkForAuxSDPLine(this);
    }

    envir().taskScheduler().doEventLoop(&fDoneFlag);

    return fAuxSDPLine;
}

FramedSource* H264VideoFifoServerMediaSubsession::createNewStreamSource(unsigned /*clientSessionId*/, unsigned& estBitrate) {
    estBitrate = 500; // kbps, estimate

    // Create the video source:
    ByteStreamFifoSource* fileSource = ByteStreamFifoSource::createNew(envir(), fFileName, 0, 50000);
    if (fileSource == NULL) return NULL;
    fFileSize = fileSource->fileSize();

    // Create a framer for the Video Elementary Stream, then a pacing
    // filter on top (delivery runs at ~15 fps wall time so greedy
    // clients cannot buffer ahead):
    FramedSource* framer = H264VideoStreamFramer::createNew(envir(), fileSource);
    return PacedFilter::createNew(envir(), framer);
}

RTPSink* H264VideoFifoServerMediaSubsession
::createNewRTPSink(Groupsock* rtpGroupsock,
                   unsigned char rtpPayloadTypeIfDynamic,
                   FramedSource* /*inputSource*/) {
    return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
}
