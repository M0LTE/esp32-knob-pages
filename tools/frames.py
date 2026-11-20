#!/usr/bin/env python3
"""Turn simulator frames (raw RGB565, 360x360) into what the round glass shows.

  frames.py <framedir> <out.gif|out.png> [--fps N] [--scale S] [--frame N]

The corners outside the 360 px circle are masked off and a thin bezel ring is
drawn so the picture reads like the real device.
"""
import argparse
import glob
import os
import struct

from PIL import Image, ImageDraw

W = H = 360


def load(path):
    raw = open(path, "rb").read()
    px = struct.unpack("<%dH" % (W * H), raw)
    img = Image.new("RGB", (W, H))
    img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in px])
    return img


def glass(img, scale):
    big = img.resize((W * scale, H * scale), Image.LANCZOS)
    pad = 18 * scale
    out = Image.new("RGB", (W * scale + 2 * pad, H * scale + 2 * pad), (24, 24, 26))
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, big.size[0] - 1, big.size[1] - 1), fill=255)
    out.paste(big, (pad, pad), mask)
    d = ImageDraw.Draw(out)
    d.ellipse((pad - 6 * scale, pad - 6 * scale, pad + W * scale + 6 * scale, pad + H * scale + 6 * scale),
              outline=(150, 60, 40), width=6 * scale)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("out")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--frame", type=int, default=-1)
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, "f*.rgb565")))
    if a.out.endswith(".png"):
        f = files[a.frame]
        glass(load(f), a.scale).save(a.out)
        return
    imgs = [glass(load(f), a.scale) for f in files]
    imgs = [i.convert("P", palette=Image.ADAPTIVE, colors=128) for i in imgs]
    imgs[0].save(a.out, save_all=True, append_images=imgs[1:], duration=int(1000 / a.fps), loop=0, optimize=True)


if __name__ == "__main__":
    main()
