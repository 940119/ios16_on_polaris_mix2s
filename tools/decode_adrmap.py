#!/usr/bin/env python3
"""Decode the ADRMAP screen photo.

The shim paints every 32-bit word of the 36 MiB splash region at 0x9d400000
with R = word>>16, G = (word>>8)&0xff. The panel therefore shows one 256 KiB
red band every 262144/stride screen rows, and the green level within a band
identifies the 1 KiB offset inside it.

Usage: decode_adrmap.py photo.jpg [out.png]
Prints the detected screen quad, red-band spacing, stride, bpp and base.
"""
import sys
import numpy as np
import cv2

REGION = 0x9d400000
SCREEN_W, SCREEN_H = 1080, 2160


def order_quad(pts):
    pts = np.asarray(pts, dtype=np.float32).reshape(4, 2)
    s = pts.sum(axis=1)
    d = np.diff(pts, axis=1).reshape(-1)
    tl = pts[np.argmin(s)]
    br = pts[np.argmax(s)]
    tr = pts[np.argmin(d)]  # x - y minimal
    bl = pts[np.argmax(d)]
    return np.array([tl, tr, br, bl], dtype=np.float32)


def find_screen(img):
    h, w = img.shape[:2]
    scale = 1400.0 / max(h, w)
    small = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(small, cv2.COLOR_BGR2HSV)
    sat, val = hsv[:, :, 1], hsv[:, :, 2]
    mask = ((sat > 45) & (val > 20)).astype(np.uint8) * 255
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((21, 21), np.uint8))
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, np.ones((7, 7), np.uint8))
    cnts, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not cnts:
        return None
    c = max(cnts, key=cv2.contourArea)
    hull = cv2.convexHull(c)
    peri = cv2.arcLength(hull, True)
    approx = cv2.approxPolyDP(hull, 0.02 * peri, True)
    if len(approx) != 4:
        approx = cv2.boxPoints(cv2.minAreaRect(c)).astype(np.int32).reshape(-1, 1, 2)
    return approx.reshape(-1, 2).astype(np.float32) / scale


def warp(img, quad):
    dst = np.array([[0, 0], [SCREEN_W - 1, 0],
                    [SCREEN_W - 1, SCREEN_H - 1], [0, SCREEN_H - 1]], np.float32)
    m = cv2.getPerspectiveTransform(order_quad(quad), dst)
    return cv2.warpPerspective(img, m, (SCREEN_W, SCREEN_H))


def red_bands(scr):
    """Return (rows, red_levels) smoothed along the centre column."""
    b, g, r = cv2.split(scr.astype(np.int16))
    col = r[:, SCREEN_W // 2 - 20:SCREEN_W // 2 + 20].mean(axis=1)
    # smooth over ~4 rows to kill JPEG noise but keep band steps
    k = np.ones(5) / 5.0
    return np.convolve(col, k, mode="same")


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "out/adrmap_rect.png"
    img = cv2.imread(path)
    if img is None:
        sys.exit("cannot read " + path)
    quad = find_screen(img)
    if quad is None:
        sys.exit("screen quad not found; pass corners manually")
    print("screen quad (TL,TR,BR,BL):\n", np.round(order_quad(quad), 1))
    scr = warp(img, quad)
    cv2.imwrite(out, scr)
    prof = red_bands(scr)
    # locate upward steps of ~1 in the red channel
    dif = np.diff(prof)
    steps = np.where(dif > 0.6)[0]
    # merge steps closer than 5 rows
    merged = []
    for s in steps:
        if not merged or s - merged[-1] > 5:
            merged.append(s)
    spacing = np.diff(merged)
    spacing = spacing[(spacing > 10) & (spacing < 400)]
    print("red steps (rows):", merged[:40])
    if len(spacing):
        med = float(np.median(spacing))
        print("median band spacing: %.1f rows -> stride %.0f bytes" %
              (med, 262144.0 / med))
    # green period along a row (256 words per green step)
    row = scr[SCREEN_H // 2, :, 1].astype(np.int16)
    row = np.convolve(row, np.ones(9) / 9.0, mode="same")
    gd = np.diff(row)
    gsteps = np.where(gd > 0.5)[0]
    gm = []
    for s in gsteps:
        if not gm or s - gm[-1] > 4:
            gm.append(s)
    gp = np.diff(gm)
    gp = gp[(gp > 20) & (gp < 600)]
    if len(gp):
        print("median green step spacing: %.1f px -> bpp %.2f" %
              (float(np.median(gp)), float(np.median(gp)) / 256.0))

    # base estimate from a patch near the screen centre
    cy, cx = SCREEN_H // 2, SCREEN_W // 2
    patch = scr[cy - 8:cy + 8, cx - 8:cx + 8].astype(np.float32)
    bgr = patch.reshape(-1, 3).mean(axis=0)  # B,G,R
    # camera white balance unknown: scale so that the full red ramp 0..143
    # maps to the observed range (max over the column)
    rmax = np.percentile(prof, 99)
    rmin = np.percentile(prof, 1)
    rr = (bgr[2] - rmin) * 143.0 / max(rmax - rmin, 1)
    word = int(round(rr)) << 16
    print("centre BGR:", np.round(bgr, 1),
          "crude word>>16:", int(round(rr)))
    addr = REGION + word * 4
    print("centre address coarse: 0x%08x" % addr)


if __name__ == "__main__":
    main()
