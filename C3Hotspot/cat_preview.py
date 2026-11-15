from PIL import Image, ImageDraw
import copy

W, H = 128, 64
S = 5  # upscale for viewing

def new():
    img = Image.new("1", (W, H), 0)
    return img, ImageDraw.Draw(img)

def tri(d, pts, v):
    d.polygon(pts, fill=v)

def rrect(d, x0, y0, x1, y1, r, v):
    d.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=v)

def cat(blink=False, gx=0, gy=0):
    img, d = new()
    W_ = 1
    # whiskers
    for a, b in [((20, 38), (2, 31)), ((20, 44), (1, 44)), ((20, 50), (3, 56)),
                 ((108, 38), (126, 31)), ((108, 44), (127, 44)), ((108, 50), (125, 56))]:
        d.line([a, b], fill=W_, width=1)
    # ears
    tri(d, [(22, 27), (30, 0), (55, 20)], 1)
    tri(d, [(106, 27), (98, 0), (73, 20)], 1)
    tri(d, [(32, 23), (35, 7), (47, 19)], 0)
    tri(d, [(96, 23), (93, 7), (81, 19)], 0)
    # head
    rrect(d, 20, 10, 107, 61, 24, 1)
    # stripes
    for x in (55, 64, 73):
        d.line([(x, 12 if x != 64 else 11), (x, 19)], fill=0, width=1)
    # eyes
    if not blink:
        d.ellipse([46-8, 33-8, 46+8, 33+8], fill=0)
        d.ellipse([82-8, 33-8, 82+8, 33+8], fill=0)
        d.ellipse([46+gx-3, 33+gy-3, 46+gx+3, 33+gy+3], fill=1)
        d.ellipse([82+gx-3, 33+gy-3, 82+gx+3, 33+gy+3], fill=1)
        d.point([(50+gx, 30+gy), (86+gx, 30+gy)], fill=1)
    else:
        d.rectangle([36, 23, 55, 41], fill=1)
        d.rectangle([72, 23, 91, 41], fill=1)
        d.rectangle([39, 33, 52, 34], fill=0)
        d.rectangle([75, 33, 88, 34], fill=0)
    # nose
    tri(d, [(59, 44), (69, 44), (64, 51)], 0)
    # mouth
    for a, b in [((64, 51), (64, 54)), ((63, 54), (58, 57)), ((58, 57), (54, 55)),
                 ((65, 54), (70, 57)), ((70, 57), (74, 55))]:
        d.line([a, b], fill=0, width=1)
    return img

frames = [cat(False, 0, 0), cat(True), cat(False, -2, 0), cat(False, 2, -1)]
sheet = Image.new("1", (W*4 + 30, H), 0)
for i, f in enumerate(frames):
    sheet.paste(f, (i*(W+10), 0))
sheet.resize((sheet.width*S, sheet.height*S), Image.NEAREST).save(
    r"C:\Users\vikas\Documents\Arduino\C3Hotspot\cat_preview.png")
print("ok")
