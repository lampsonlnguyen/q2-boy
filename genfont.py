"""Rasterize the included, licensed font into small pixel masks."""
from PIL import Image, ImageDraw, ImageFont

font = ImageFont.truetype("LiberationSans-Regular.ttf", 15)
print("static const unsigned short glyphs[95][17] = {")
for char in range(32, 127):
    image = Image.new("L", (12, 17))
    ImageDraw.Draw(image).text((0, 0), chr(char), font=font, fill=255)
    rows = [sum((image.getpixel((x, y)) >= 105) << x for x in range(12)) for y in range(17)]
    print("    {" + ", ".join(hex(row) for row in rows) + "},")
print("};")
print("static const unsigned char advance[95] = {")
print("    " + ", ".join(str(max(3, round(font.getlength(chr(c))))) for c in range(32, 127)))
print("};")
