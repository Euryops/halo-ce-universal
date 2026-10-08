#!/usr/bin/env python3
"""Checks Dolphin's frame dumps of skintest.dol (skintest_scene.c).

    check.py <Dump/Frames folder> [out.png]

The check phase's frames are split: the left half skinned by the game's
vertex programs, run on the Wii's CPU, the right half by the game's own
debug formula on the CPU and drawn with one identity node. Each check frame's
halves are compared pixel by pixel, in 255ths of the largest of r, g and b.
The frames pass when, over all of them, at most one pixel in two hundred is
off by more than 16 and the mean is under 1: what is left is the reference's
normals rounded to NORMPACKED3 and the odd pixel on a triangle's edge.

The picture is the last frame of the scene above, and the last check frame
with the difference of its halves, eight times over, beside it. Needs
Pillow. With DOLPHIN_FRAME_DUMP_RAW=1 the frames are 640x480 PNGs; .raw
frames of that size are read as well."""

import glob
import os
import sys

from PIL import Image, ImageChops

WIDTH, HEIGHT = 640, 480
HALF = WIDTH // 2
TOLERANCE = 16


def frame_number(path):
    return int(os.path.basename(path).split('_')[-1].split('.')[0])


def pixels(image):
    """the pixels in order (getdata is going from Pillow)"""
    return image.get_flattened_data() if hasattr(image, 'get_flattened_data') else image.getdata()


def load(path):
    if path.endswith('.raw'):
        with open(path, 'rb') as file:
            return Image.frombytes('RGB', (WIDTH, HEIGHT), file.read())
    image = Image.open(path).convert('RGB')
    if image.size != (WIDTH, HEIGHT):
        image = image.resize((WIDTH, HEIGHT), Image.NEAREST)
    return image


def phase(image):
    """'scene' or 'check' by the mark in the top left corner, or None"""
    first = image.getpixel((4, 4))
    second = image.getpixel((11, 4))
    dark = lambda p: max(p) < 40
    light = lambda p: min(p) > 215
    if light(first) and dark(second):
        return 'scene'
    if dark(first) and light(second):
        return 'check'
    return None


def compare(image):
    """worst, the sum and the count of pixels off, of a frame's two halves"""
    left = pixels(image.crop((0, 0, HALF, HEIGHT)))
    right = pixels(image.crop((HALF, 0, WIDTH, HEIGHT)))
    worst, total, off = 0, 0, 0
    for p, q in zip(left, right):
        d = max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))
        worst = max(worst, d)
        total += d
        off += d > TOLERANCE
    return worst, total, off


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    paths = sorted(glob.glob(os.path.join(sys.argv[1], 'framedump_*.png')) +
                   glob.glob(os.path.join(sys.argv[1], 'framedump_*.raw')), key=frame_number)
    scene, checks, previous = None, [], None
    for path in paths:
        image = load(path)
        kind = phase(image)
        if kind == 'scene':
            scene = image
        elif kind == 'check':
            # Dolphin dumps a frame for each field it shows: the same
            # picture twice is one frame
            if previous is not None and not ImageChops.difference(image, previous).getbbox():
                continue
            checks.append((path, image))
            previous = image
    if not checks:
        print('no check frames among %d dumps' % len(paths))
        return 1
    worst_all, total_all, off_all = 0, 0, 0
    for path, image in checks:
        worst, total, off = compare(image)
        worst_all = max(worst_all, worst)
        total_all += total
        off_all += off
    count = len(checks) * HALF * HEIGHT
    mean = total_all / count
    share = off_all / count
    passed = share <= 0.005 and mean < 1.0
    print('%d check frames: worst %d/255, mean %.3f, %.4f%% of pixels off by more than %d: %s' %
          (len(checks), worst_all, mean, 100.0 * share, TOLERANCE, 'PASS' if passed else 'FAIL'))
    if len(sys.argv) > 2:
        last = checks[-1][1]
        difference = ImageChops.difference(last.crop((0, 0, HALF, HEIGHT)), last.crop((HALF, 0, WIDTH, HEIGHT)))
        difference = difference.point(lambda value: min(255, value * 8))
        picture = Image.new('RGB', (WIDTH + HALF, HEIGHT * 2), (40, 40, 40))
        if scene is not None:
            picture.paste(scene, (HALF // 2, 0))
        picture.paste(last, (0, HEIGHT))
        picture.paste(difference, (WIDTH, HEIGHT))
        picture.save(sys.argv[2])
        print('wrote %s: the scene above; below, the check (left: the programs, right: the reference) and its'
              ' difference x8' % sys.argv[2])
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
