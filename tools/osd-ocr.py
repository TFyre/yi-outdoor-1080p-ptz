#!/usr/bin/env python3
"""OCR the camera's burned-in OSD clock from a video and report the
timestamp sequence - including every backward jump - so stream quality
can be measured objectively instead of eyeballed.

Usage: python3 tools/osd-ocr.py <video> [--region x,y,w,h] [--fps 1]

Frames are extracted with ffmpeg at --fps, OCR'd with tesseract. With
no --region, the tool probes candidate boxes across the top of the
frame on the first frames and locks onto the one that parses as a
clock. Output: frame_number  timestamp, and a JUMP report at the end.

Run inside WSL (needs ffmpeg, tesseract-ocr, python3-pil).
"""
import subprocess, sys, re, os, tempfile, argparse

# The OSD prints the date immediately before the time
# ("2026082515:45:25"), so the time has no word boundary on the left.
TS_RE = re.compile(r'(\d{1,2}:\d{2}:\d{2})')

def ocr_box(img_path, box):
    """OCR one cropped box; return the first HH:MM:SS found. The RAW
    crop is what works (calibrated on analysis/frame30.png: tesseract's
    own thresholding reads the digits cleanly; any pre-binarization
    destroyed them)."""
    from PIL import Image, ImageOps
    im = Image.open(img_path).convert('L').crop(box)
    tmp = tempfile.mktemp(suffix='.png')
    try:
        for name, v in (('raw', im),
                        ('x3', im.resize((im.width * 3, im.height * 3),
                                         Image.LANCZOS)),
                        ('x3c', ImageOps.autocontrast(
                            im.resize((im.width * 3, im.height * 3),
                                      Image.LANCZOS)))):
            v.save(tmp)
            out = subprocess.run(
                ['tesseract', tmp, 'stdout', '--psm', '7',
                 '-c', 'tessedit_char_whitelist=0123456789:/'],
                capture_output=True, text=True, timeout=10).stdout
            m = TS_RE.search(out)
            if m:
                return m.group(1)
    finally:
        os.unlink(tmp)
    return None

def to_secs(ts):
    h, m, s = map(int, ts.split(':'))
    return h * 3600 + m * 60 + s

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('--region', help='x,y,w,h (empty = auto-detect)')
    ap.add_argument('--fps', type=int, default=1)
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix='osdocr')
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', args.video,
                    '-vf', 'fps=%d' % args.fps,
                    os.path.join(work, 'f%05d.png')], check=True)
    frames = sorted(os.listdir(work))

    from PIL import Image
    W, H = Image.open(os.path.join(work, frames[0])).size

    # The known OSD box: top-left, 500x65 at 1920x1080. When the video
    # is a different size, scale the box with the width. No probing:
    # the stream's head frames can be error-laden without a rendered
    # OSD, and rejecting there loses the readable frames that follow.
    if args.region:
        x, y, w, h = map(int, args.region.split(','))
        box = (x, y, x + w, y + h)
    else:
        s = W / 1920.0
        bw, bh = int(500 * s), int(65 * s)
        box = (0, 0, bw, bh)
    print("# OSD box:", box)

    seq = []
    jumps = []
    prev = None
    for f in frames:
        ts = ocr_box(os.path.join(work, f), box)
        if not ts:
            continue
        secs = to_secs(ts)
        idx = int(f[1:6])
        seq.append((idx, secs))
        print("%s  %s" % (f, ts))
        if prev is not None and secs < prev[1] and prev[1] - secs > 1:
            jumps.append((prev[0], prev[1], idx, secs, prev[1] - secs))
        prev = (idx, secs)

    print("\n# %d frames with a readable clock, %d backward jumps:"
          % (len(seq), len(jumps)))
    for j in jumps:
        print("  frame %d (%d s) -> frame %d (%d s): BACKWARD %d s"
              % (j[0], j[1], j[2], j[3], j[4]))

if __name__ == '__main__':
    main()
