#!/usr/bin/env python3
"""
M130 chroma oracle.

Companion to mz0380-m127-splash.py. That one answers "is this the card's
standby logo"; this one answers "is the colour right", which is the only thing
still wrong with the picture after M129.

The defect it exists to measure: the captured frame's luma is perfect and its
chroma planes are ~96% ANTI-correlated with luma. That is the signature of an
RGB->YCbCr matrix being applied to a YCbCr 4:4:4 input - HDMI puts Cb on the
blue channel, Y on green, Cr on red, so an RGB matrix computes
Cb_out = -0.291*Y + ..., i.e. mostly inverted luma. Correct colour-difference
planes are close to UNcorrelated with luma and sit near 128.

    corr(chroma, luma) near  0   -> healthy
    corr(chroma, luma) near -1   -> the CSC is converting when it should not

Fixed in M130 by mst_csc_ctl (default AUTO), which picks the CSC mode from the
colour space the source is actually sending. This script stays as the check.

Layout is planar 4:2:0 (Y, then two W/2 x H/2 planes) - NOT NV12, despite the
capture script's file extension. Decoding it as NV12 produces magenta/green
interleave banding.

Exit status:  0 = chroma looks correct
              1 = chroma is luma-contaminated (the M129 defect)
              2 = cannot read a whole frame
"""

import sys

W, H = 1920, 1080
Y_SIZE = W * H
C_W, C_H = W // 2, H // 2
C_SIZE = C_W * C_H
FRAME = Y_SIZE + 2 * C_SIZE

# The honest statistic is corr^2: the fraction of a chroma plane's variance
# that luma alone explains. Correlation magnitude on its own is a bad test,
# because real scenes DO correlate colour with brightness - a bright red object
# is bright and red. What is pathological is a chroma plane that is almost
# nothing BUT luma.
#
# Calibration, both measured on this hardware:
#   M129, CSC converting a YUV444 source as RGB:  A 0.93, B 0.56   (broken)
#   M130, mst_csc_ctl=0 on the same scene:        A 0.10, B 0.12   (correct
#                                                  colour, confirmed visually)
# The two are an order of magnitude apart, so the thresholds are not delicate.
CONTAMINATED = 0.40     # >= this share of chroma is luma -> the CSC bug
CLEAN = 0.25            # <= this -> normal scene correlation


def corr(a, b):
    n = len(a)
    ma = sum(a) / n
    mb = sum(b) / n
    sab = sxx = syy = 0.0
    for x, y in zip(a, b):
        dx = x - ma
        dy = y - mb
        sab += dx * dy
        sxx += dx * dx
        syy += dy * dy
    if sxx <= 0 or syy <= 0:
        return 0.0
    return sab / (sxx * syy) ** 0.5


def stats(p):
    n = len(p)
    m = sum(p) / n
    var = sum((v - m) ** 2 for v in p) / n
    return m, var ** 0.5


def main(path):
    try:
        with open(path, "rb") as f:
            d = f.read(FRAME)
    except OSError as e:
        print(f"cannot read {path}: {e}")
        return 2
    if len(d) < FRAME:
        print(f"{path}: {len(d)} bytes, need {FRAME} for one 1920x1080 frame")
        return 2

    y = d[:Y_SIZE]
    a = d[Y_SIZE:Y_SIZE + C_SIZE]
    b = d[Y_SIZE + C_SIZE:FRAME]

    # Luma downsampled to chroma resolution, sampling one pixel per 2x2 block.
    # Subsample further for speed - correlation this strong needs no more.
    step = 7
    yd, ad, bd = [], [], []
    for r in range(0, C_H, step):
        base = 2 * r * W
        crow = r * C_W
        for c in range(0, C_W, step):
            yd.append(y[base + 2 * c])
            ad.append(a[crow + c])
            bd.append(b[crow + c])

    ca = corr(ad, yd)
    cb = corr(bd, yd)
    ma, sa = stats(ad)
    mb, sb = stats(bd)
    my, sy = stats(yd)

    print(f"file      {path} ({len(d)} bytes, planar 4:2:0)")
    print(f"samples   {len(yd)}")
    print(f"luma      mean {my:6.2f}  std {sy:5.2f}")
    print(f"plane A   mean {ma:6.2f}  std {sa:5.2f}   corr(A, luma) = {ca:+.4f}")
    print(f"plane B   mean {mb:6.2f}  std {sb:5.2f}   corr(B, luma) = {cb:+.4f}")
    print()

    ra, rb = ca * ca, cb * cb
    worst = max(ra, rb)
    print(f"share of chroma explained by luma:  A {ra:.2f}   B {rb:.2f}"
          f"   (broken ~0.93, healthy ~0.10)")
    print()

    if worst >= CONTAMINATED:
        print(f"VERDICT: CHROMA CONTAMINATED - luma explains {worst:.0%} of a")
        print("         chroma plane. The MST3367 CSC is converting a YCbCr")
        print("         input as if it were RGB. Fix: mst_csc_ctl=0, or leave")
        print("         it at AUTO and check the input colorspace was detected")
        print("         (the run logs 'MST3367 CSC 0x92 = ...').")
        return 1
    if worst <= CLEAN:
        print(f"VERDICT: CHROMA OK - luma explains at most {worst:.0%} of a")
        print("         chroma plane, which is ordinary for a real scene.")
        print("         View it - the layout is I420, NOT nv12:")
        print(f"         ffplay -f rawvideo -pixel_format yuv420p "
              f"-video_size {W}x{H} {path}")
        return 0
    print(f"VERDICT: PARTIAL - luma explains {worst:.0%} of a chroma plane,")
    print(f"         between clean ({CLEAN:.0%}) and contaminated "
          f"({CONTAMINATED:.0%}).")
    print("         Better than the M129 baseline but not obviously right.")
    print("         Look at it before drawing a conclusion.")
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <raw frame>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
