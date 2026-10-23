#!/usr/bin/env python3
"""Regenerate the checked-in multi-size icon (optional: python -m pip install Pillow).

The native vector design is XrGate.svg. Pillow draws the same simple glasses
silhouette at 4x resolution; CI uses the committed ICO and needs no image tools.
"""
from pathlib import Path
from PIL import Image, ImageDraw

scale = 4
image = Image.new("RGBA", (256 * scale, 256 * scale))
draw = ImageDraw.Draw(image)
def points(values):
    return [(x * scale, y * scale) for x, y in values]
def box(values):
    return tuple(value * scale for value in values)
draw.rounded_rectangle(box((8, 8, 248, 248)), radius=56*scale, fill="#102235")
for line in (((43,105),(26,76)), ((213,105),(230,76))):
    draw.line(points(line), fill="#83a3c7", width=15*scale)
    for x,y in line: draw.ellipse(box((x-7.5,y-7.5,x+7.5,y+7.5)), fill="#83a3c7")
draw.polygon(points(((41,94),(215,94),(225,99),(227,109),(216,159),(212,172),(195,178),(160,178),(147,172),(140,163),(135,149),(128,142),(121,149),(116,163),(109,174),(96,178),(61,178),(46,172),(40,159),(29,109),(31,99))), fill="#37d6bd")
for lens in (((52,111),(101,111),(110,115),(110,120),(105,148),(101,156),(94,158),(64,158),(57,155),(54,148),(48,120),(48,115)),
             ((155,111),(204,111),(209,115),(208,120),(202,148),(199,155),(192,158),(162,158),(155,155),(151,148),(146,120),(147,115))):
    draw.polygon(points(lens), fill="#0c1526")
for x in (62,164):
    draw.rounded_rectangle(box((x,117.5,x+26,122.5)), radius=2.5*scale, fill="#93fff0")
image = image.resize((256,256), Image.Resampling.LANCZOS)
image.save(Path(__file__).with_name("XrGate.ico"), sizes=[(size,size) for size in (16,24,32,48,64,128,256)])
