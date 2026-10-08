#!/usr/bin/env python3
"""Checks Dolphin's frame dumps of tevtest.dol (the shader sheet, tev_sheet.h)
against what the sheet should look like (tev_sheet_expect.c's images).

    check.py <Dump/Frames folder> <tev_sheet_expect folder> [out.png]

Each tile's pixels are compared with the NV2A's (its pixel shader as the
Xbox computes it) and with the TEV model's (the translation as Dolphin's
software renderer computes it), in 255ths; a tile is off when more than one
pixel in a hundred is off by more than 6 from the NV2A's. The picture is the
sheet as Dolphin drew it, the NV2A's, and their difference eight times over,
the rgb above and the alpha below. Needs Pillow."""

import glob
import os
import sys

from PIL import Image, ImageChops

WIDTH, HEIGHT = 640, 480
COLUMNS, TILE, PITCH, TOP = 16, 32, 40, 40
TOLERANCE = 6


def frame_number(path):
    return int(os.path.basename(path).split('_')[-1].split('.')[0])


def pixels(image):
    """the pixels in order (getdata is going from Pillow)"""
    return image.get_flattened_data() if hasattr(image, 'get_flattened_data') else image.getdata()


def load_raw(path):
    with open(path, 'rb') as file:
        return Image.frombytes('RGB', (WIDTH, HEIGHT), file.read())


def distance(a, b):
    """the sum of the absolute differences, over a few rows, to tell frames apart"""
    difference = ImageChops.difference(a, b).convert('L').resize((160, 120))
    return sum(pixels(difference))


def tile_origin(tile):
    gap = (PITCH - TILE) // 2
    return (tile % COLUMNS) * PITCH + gap, TOP + (tile // COLUMNS) * PITCH + gap


def compare(dump, expected, tile):
    """worst, mean and the share of pixels off, of a tile, over r, g and b"""
    x0, y0 = tile_origin(tile)
    box = (x0, y0, x0 + TILE, y0 + TILE)
    a = pixels(dump.crop(box))
    b = pixels(expected.crop(box))
    worst, total, off = 0, 0, 0
    for p, q in zip(a, b):
        d = max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))
        worst = max(worst, d)
        total += d
        off += d > TOLERANCE
    return worst, total / (TILE * TILE), off / (TILE * TILE)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    frames_folder, expected_folder = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else 'sheet.png'
    expected = {name: load_raw(os.path.join(expected_folder, name + '.raw'))
                for name in ('nv2a_rgb', 'nv2a_alpha', 'tev_rgb', 'tev_alpha')}
    tiles = []
    with open(os.path.join(expected_folder, 'tiles.txt')) as file:
        for line in file:
            number, stages, name = line.rstrip('\n').split(' ', 2)
            tiles.append((int(number), int(stages), name))

    # the frame of each view nearest what it should be, among every fifth
    frames = sorted(glob.glob(os.path.join(frames_folder, '*.png')), key=frame_number)
    if not frames:
        sys.exit('no frames in ' + frames_folder)
    best = {}
    for path in frames[::5]:
        image = Image.open(path).convert('RGB')
        if image.size != (WIDTH, HEIGHT):
            sys.exit('%s is %dx%d: dump the frames raw (DOLPHIN_FRAME_DUMP_RAW=1)' % ((path,) + image.size))
        for view in ('rgb', 'alpha'):
            d = distance(image, expected['nv2a_' + view])
            if view not in best or d < best[view][0]:
                best[view] = (d, path, image)

    off_tiles = []
    worst_overall = {'rgb': 0, 'alpha': 0}
    print('frames: rgb %s, alpha %s' % (os.path.basename(best['rgb'][1]), os.path.basename(best['alpha'][1])))
    print('tile stages  rgb: worst mean off%% (TEV model worst)   alpha: worst mean off%% (TEV model worst)  name')
    for number, stages, name in tiles:
        if not stages:
            print('%4d  refused  %s' % (number, name))
            continue
        line = '%4d %6d ' % (number, stages)
        tile_off = False
        for view in ('rgb', 'alpha'):
            dump = best[view][2]
            worst, mean, off = compare(dump, expected['nv2a_' + view], number)
            model_worst = compare(dump, expected['tev_' + view], number)[0]
            worst_overall[view] = max(worst_overall[view], worst)
            line += '  %3d %5.2f %5.1f%% (%3d)       ' % (worst, mean, 100 * off, model_worst)
            tile_off |= off > 0.01
        print(line + ('OFF ' if tile_off else '    ') + name)
        if tile_off:
            off_tiles.append(number)
    translated = sum(1 for t in tiles if t[1])
    print('%d tiles, %d translated, %d refused (magenta); %d off: %s' % (
        len(tiles), translated, len(tiles) - translated, len(off_tiles), off_tiles))

    sheet = Image.new('RGB', (3 * WIDTH, 2 * HEIGHT))
    for row, view in enumerate(('rgb', 'alpha')):
        dump = best[view][2]
        reference = expected['nv2a_' + view]
        difference = ImageChops.difference(dump, reference).point(lambda v: min(255, v * 8))
        for column, image in enumerate((dump, reference, difference)):
            sheet.paste(image, (column * WIDTH, row * HEIGHT))
    sheet.save(out)
    print('wrote', out)
    return 1 if off_tiles else 0


if __name__ == '__main__':
    sys.exit(main())
