"""dof_montage.py <out.png> <label=ppm> ...: captures side by side, two to a row, each scaled to 640 wide with its label
above it, for the depth of field's comparison (milestone 14, step 5). Local only."""

import sys

from PIL import Image, ImageDraw

WIDTH = 640
LABEL = 20
out = sys.argv[1]
tiles = []
for arg in sys.argv[2:]:
    label, path = arg.split("=", 1)
    image = Image.open(path).convert("RGB")
    image = image.resize((WIDTH, round(image.size[1] * WIDTH / image.size[0])))
    tiles.append((label, image))
height = max(image.size[1] for _, image in tiles) + LABEL
rows = (len(tiles) + 1) // 2
sheet = Image.new("RGB", (2 * WIDTH, rows * height), (16, 16, 16))
draw = ImageDraw.Draw(sheet)
for i, (label, image) in enumerate(tiles):
    x, y = (i % 2) * WIDTH, (i // 2) * height
    draw.text((x + 6, y + 4), label, fill=(240, 240, 240))
    sheet.paste(image, (x, y + LABEL))
sheet.save(out)
print(out, sheet.size)
