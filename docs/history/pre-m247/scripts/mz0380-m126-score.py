#!/usr/bin/env python3
"""Score an m55 capture by CONTENT, not by file size.

The splash is monochrome: the whole UV plane holds one value. Any real source
frame - even a black one from a live HDMI input - is extremely unlikely to
leave 1036800 chroma samples byte-identical. That single number is the oracle.

Usage: mz0380-m126-score.py [/tmp/cap-m55.nv12] [width] [height]
"""
import sys, collections

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cap-m55.nv12"
W = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
H = int(sys.argv[3]) if len(sys.argv) > 3 else 1080

d = open(path, "rb").read()
need = W * H * 3 // 2
if len(d) < need:
    sys.exit("%s: %d bytes, need %d for %dx%d I420" % (path, len(d), need, W, H))
y, uv = d[:W * H], d[W * H:need]

yh = collections.Counter(y).most_common(3)
uvu = len(set(uv))
rows = [r for r in range(H) if len(set(y[r * W:(r + 1) * W])) > 1]

print("file      %s (%d bytes, %d whole frames)" % (path, len(d), len(d) // need))
print("Y  top    %s" % ", ".join("%3d x%d" % (v, n) for v, n in yh))
print("Y  unique %d, non-flat rows %d%s" %
      (len(set(y)), len(rows),
       "" if not rows else " (%d..%d)" % (rows[0], rows[-1])))
print("UV unique %d" % uvu)
print()
if uvu == 1:
    print("VERDICT: SPLASH - chroma plane is one value; the VIC wrote no live pixels.")
else:
    print("VERDICT: NOT THE SPLASH - chroma varies. Look at the frame:")
    # M130: planar I420, not NV12 - decoding it as nv12 gives the magenta/green
    # interleave banding rather than the picture.
    print("  ffplay -f rawvideo -pixel_format yuv420p -video_size %dx%d %s" % (W, H, path))
