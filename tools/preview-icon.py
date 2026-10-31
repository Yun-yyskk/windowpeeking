
from PIL import Image
im = Image.open("assets/icon.ico")
im.size = (256, 256); im.load()
sheet = Image.new("RGBA", (256 + 3*48 + 40, 256), (245, 246, 248, 255))
sheet.paste(im.convert("RGBA"), (0, 0), im.convert("RGBA"))
x = 276
for s in (48, 32, 16):
    t = Image.open("assets/icon.ico"); t.size = (s, s); t.load()
    sheet.paste(t.convert("RGBA"), (x, 20), t.convert("RGBA"))
    x += s + 12
sheet.save("build/icon-preview.png")
print("ok")
