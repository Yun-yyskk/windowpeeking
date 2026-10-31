
import os
from PIL import Image, ImageDraw

SS = 8           # supersample factor
N  = 256
S  = N * SS

ring  = (47, 129, 247, 255)    # #2F81F7
ring2 = (23, 76, 155, 255)     # darker outer edge for depth
glass = (12, 24, 41, 255)      # deep navy "glass"
gloss = (120, 190, 255, 150)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

pad = S * 0.045
outer = [pad, pad, S - pad, S - pad]
d.ellipse(outer, fill=ring2)
inset = S * 0.012
d.ellipse([outer[0] + inset, outer[1] + inset, outer[2] - inset, outer[3] - inset], fill=ring)

hole_pad = S * 0.175
hole = [hole_pad, hole_pad, S - hole_pad, S - hole_pad]
d.ellipse(hole, fill=glass)

# a highlight arc on the glass, upper left
w = max(1, int(S * 0.035))
d.arc([hole[0] + S*0.055, hole[1] + S*0.055, hole[2] - S*0.055, hole[3] - S*0.055],
      200, 300, fill=gloss, width=w)

img = img.resize((N, N), Image.LANCZOS)
os.makedirs("assets", exist_ok=True)
img.save("assets/icon.ico", format="ICO", sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
print("ico bytes", os.path.getsize("assets/icon.ico"))
for s in (16, 24, 32):
    im = Image.open("assets/icon.ico"); im.size = (s, s); im.load()
    px = im.convert("RGBA").getpixel((s//2, s//2))
    print(s, "centre", px, "corner", im.convert("RGBA").getpixel((0,0)))
