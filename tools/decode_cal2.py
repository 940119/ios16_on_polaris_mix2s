#!/usr/bin/env python3
"""Decode the CAL2 photo: absolute splash-scanout base.

Pattern painted over 0x9d400000..0x9f800000 (region-relative word w):
  w>>16 = 256 KiB block index k; each block starts with 8 uniform rows whose
  colour is bit k[7-row] (white=1, black=0);
  rows at (w & 0x3fff) < 1080 are white rulers (every 64 KiB);
  word w with (w mod 1080)==0 is cyan (constant screen x = base % 4320).
"""
import sys
import numpy as np
import cv2

REGION = 0x9d400000
W, H = 1080, 2160
STRIDE = 4320


def find_screen(img):
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    gx = cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3)
    mag = cv2.magnitude(gx, gy)
    m = (mag > 60).astype(np.uint8) * 255
    m = cv2.dilate(m, np.ones((9, 9), np.uint8), iterations=1)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((61, 61), np.uint8))
    cnts, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not cnts:
        return None
    c = max(cnts, key=cv2.contourArea)
    hull = cv2.convexHull(c)
    peri = cv2.arcLength(hull, True)
    ap = cv2.approxPolyDP(hull, 0.02 * peri, True)
    if len(ap) != 4:
        ap = cv2.boxPoints(cv2.minAreaRect(c)).astype(np.int32).reshape(-1, 1, 2)
    return ap.reshape(-1, 2).astype(np.float32)


def order_for_portrait(pts):
    """Return TL,TR,BR,BL such that the long physical edge is vertical."""
    pts = np.asarray(pts, np.float32).reshape(4, 2)
    # order around the quadrilateral
    c = pts.mean(axis=0)
    ang = np.arctan2(pts[:, 1] - c[1], pts[:, 0] - c[0])
    pts = pts[np.argsort(ang)]
    # rotate so that the longest edge starts at index 0
    d = [np.linalg.norm(pts[i] - pts[(i + 1) % 4]) for i in range(4)]
    i0 = int(np.argmax(d))  # long edge pts[i0] -> pts[i0+1]
    pts = np.roll(pts, -i0, axis=0)
    # pts[0]->pts[1] is long; make it the left edge running down
    tl, tr, br, bl = pts[0], pts[3], pts[2], pts[1]
    return np.array([tl, tr, br, bl], np.float32)


def warp(img, quad):
    dst = np.array([[0, 0], [W - 1, 0], [W - 1, H - 1], [0, H - 1]], np.float32)
    m = cv2.getPerspectiveTransform(order_for_portrait(quad), dst)
    return cv2.warpPerspective(img, m, (W, H))


def uniform_rows(scr):
    b, g, r = cv2.split(scr.astype(np.int16))
    # exclude the cyan column and a margin
    cyan = (b > 130) & (g > 130) & (r < 110)
    xs = np.where(cyan.sum(axis=0) > H // 4)[0]
    xc = int(np.median(xs)) if len(xs) else -1
    keep = np.ones(W, bool)
    if xc >= 0:
        keep[max(0, xc - 8):min(W, xc + 9)] = False
    lum = (0.114 * b + 0.587 * g + 0.299 * r)[:, keep]
    mean = lum.mean(axis=1)
    white = (lum > 185).mean(axis=1)
    black = (lum < 45).mean(axis=1)
    return mean, white, black, xc


def main():
    path = sys.argv[1]
    img = cv2.imread(path)
    if img is None:
        sys.exit("cannot read " + path)
    quad = find_screen(img)
    if quad is None:
        sys.exit("screen not found")
    scr = warp(img, quad)
    cv2.imwrite("out/cal2_rect.png", scr)
    mean, white, black, xc = uniform_rows(scr)
    print("cyan column x =", xc)
    unif = (white > 0.75) | (black > 0.75)
    # runs of uniform rows
    runs, s = [], None
    for i, u in enumerate(unif):
        if u and s is None:
            s = i
        elif not u and s is not None:
            runs.append((s, i - 1))
            s = None
    if s is not None:
        runs.append((s, H - 1))
    codes = []
    for a, b in runs:
        if b - a + 1 < 6:
            continue
        bits = 0
        for j in range(8):
            y = a + j
            if y > H - 1:
                break
            bit = 1 if white[y] > black[y] else 0
            bits = (bits << 1) | bit
        codes.append((a, b, bits))
    print("code runs (y0,y1,block index):")
    for c in codes:
        print(c)
    if not codes:
        sys.exit("no code bands")
    # pick the best-formed run (exactly 8)
    a, b, k = max(codes, key=lambda t: (t[1] - t[0] == 7, -t[0]))
    if b - a + 1 != 8:
        k = codes[0][2]
        a = codes[0][0]
    print("using block %d at screen row %d" % (k, a))
    brel = k * 262144 - a * STRIDE
    print("B_rel coarse = 0x%x (%d)" % (brel, brel))
    if xc >= 0:
        target = (-4 * xc) % STRIDE
        best = None
        for d in range(-3, 4):
            cand = brel + d * STRIDE
            adj = (target - cand) % STRIDE
            if adj > STRIDE // 2:
                adj -= STRIDE
            score = abs(adj)
            if best is None or score < best[0]:
                best = (score, cand + adj)
        print("cyan target mod 4320 = %d, adjusted base = %d (0x%x)" %
              (target, best[1], best[1]))
        brel = best[1]
    print("absolute base = 0x%08x" % (REGION + brel))
    print("region offset = 0x%x (%.2f MiB)" % (brel, brel / 1048576.0))


if __name__ == "__main__":
    main()
