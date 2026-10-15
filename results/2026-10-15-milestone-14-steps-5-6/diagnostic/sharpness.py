"""sharpness.py <ppm> [...]: each capture's mean absolute horizontal and vertical step in green, over bands of its
height (top, middle, bottom fifths of the frame), a rough measure of blur for setting depth of field side by side.
Milestone 14, step 5; local only."""

import sys

from PIL import Image


def band_sharpness(image, top, bottom):
    pixels = image.load()
    width = image.size[0]
    total = 0
    count = 0
    for y in range(top, bottom - 1):
        for x in range(0, width - 1, 2):
            g = pixels[x, y][1]
            total += abs(g - pixels[x + 1, y][1]) + abs(g - pixels[x, y + 1][1])
            count += 2
    return total / count


for path in sys.argv[1:]:
    image = Image.open(path).convert("RGB")
    height = image.size[1]
    bands = [band_sharpness(image, height * i // 5, height * (i + 1) // 5) for i in range(5)]
    print(f"{path}: " + " ".join(f"{b:5.2f}" for b in bands))
