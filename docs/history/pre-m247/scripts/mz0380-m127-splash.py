#!/usr/bin/env python3
"""Decide whether an m55 capture is the card's NO SIGNAL splash - by identity.

M126 scored this on the chroma plane ("UV unique == 1"), which is a heuristic:
it says "no live pixels", not "this is the splash". M127 identified the picture
exactly. It is NOSG_LOGO_Y, a 320x240 Y-only data symbol inside the card's
tinyvenc5 (ELF vaddr 0x6b10c, 0x12c00 bytes), blitted dead-centre into an
otherwise 0x11 (video black) 1920x1080 frame by EncodingGroup::fake_frame_process:

    dst x = (1920 - 320) / 2 = 800
    dst y = (1080 - 240) / 2 = 420
    dst stride = 1920, 320 bytes per row, 240 rows

So the test is a SHA-256 of that crop. The asset itself is vendor firmware and
is deliberately NOT vendored into this tree - only its digest is, which is
enough to recognise it and nothing more.

Three outcomes, and the third is the one worth having:

    SPLASH          - crop hashes to the known asset. The VIC saw no signal and
                      the card drew its own picture. Nothing changed.
    NOT SPLASH      - the frame is something else. Whatever the run varied,
                      it moved the picture. Look at it.
    NO FRAME        - short file. The encoder never rendered; usually means the
                      run broke tinyvenc5 rather than the capture path.

Usage: mz0380-m127-splash.py [/tmp/cap-m55.nv12] [width] [height]
"""
import hashlib
import sys

# SHA-256 of tinyvenc5's NOSG_LOGO_Y, 320x240 bytes, verified byte-identical
# against a real capture on 2026-08-21 (RE_FINDINGS.md M127b).
NOSG_LOGO_Y_SHA256 = \
    "dfce4efd5139298f544d23473f85a42fb7115a3c5e4ba65b71c070d59883a30b"
LOGO_W, LOGO_H = 320, 240

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cap-m55.nv12"
W = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
H = int(sys.argv[3]) if len(sys.argv) > 3 else 1080

data = open(path, "rb").read()
need = W * H * 3 // 2
print("file      %s (%d bytes, %d whole frames)" % (path, len(data), len(data) // need if need else 0))

if len(data) < need:
    print("\nVERDICT: NO FRAME - %d bytes, need %d for %dx%d I420" % (len(data), need, W, H))
    sys.exit(2)

y = data[:W * H]
x0, y0 = (W - LOGO_W) // 2, (H - LOGO_H) // 2
crop = b"".join(y[(y0 + r) * W + x0:(y0 + r) * W + x0 + LOGO_W] for r in range(LOGO_H))
digest = hashlib.sha256(crop).hexdigest()

print("crop      %dx%d at (%d,%d)" % (LOGO_W, LOGO_H, x0, y0))
print("sha256    %s" % digest)

if digest == NOSG_LOGO_Y_SHA256:
    print("\nVERDICT: SPLASH - byte-identical to tinyvenc5's NOSG_LOGO_Y.")
    print("         The VIC reported no signal; fake_frame_process drew this.")
    sys.exit(1)

# Not the known asset. Say as much as possible about what it is instead, because
# this is the branch the whole project is trying to reach.
outside = [v for i, v in enumerate(y)
           if not (y0 <= i // W < y0 + LOGO_H and x0 <= i % W < x0 + LOGO_W)]
uv = data[W * H:need]
print("\nVERDICT: NOT SPLASH - the centre crop is not the known asset.")
print("         Y outside the crop: %d distinct values" % len(set(outside)))
print("         UV plane:           %d distinct values" % len(set(uv)))
# M130: the payload is planar I420, not NV12. This hint said nv12 and so
# reproduced the exact viewing mistake that cost two sessions - and it is the
# line a reader sees at the moment they have a real frame in hand.
print("         View it:  ffplay -f rawvideo -pixel_format yuv420p "
      "-video_size %dx%d %s" % (W, H, path))
sys.exit(0)
