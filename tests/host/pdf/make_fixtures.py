#!/usr/bin/env python3
"""Generate deterministic PDF fixtures for tests/host/pdf/probe.cpp.

Usage: make_fixtures.py OUT_DIR [JPEG_PATH]

Only the Python standard library (zlib) is used. The DCT fixture embeds
tests/host/jpeg/fixture.jpg so the probe can compare extracted bytes.
"""
import os
import struct
import sys
import zlib


def jpeg_dims(data):
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xD8, 0xD9):
            i += 2
            continue
        seglen = struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE,
                      0xCF):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + seglen
    raise ValueError("no SOF in jpeg")


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def png_encode(rows, bpp, filters):
    prev = bytes(len(rows[0]))
    out = bytearray()
    for idx, row in enumerate(rows):
        f = filters[idx % len(filters)]
        enc = bytearray()
        for j, v in enumerate(row):
            a = row[j - bpp] if j >= bpp else 0
            b = prev[j]
            c = prev[j - bpp] if j >= bpp else 0
            if f == 0:
                d = v
            elif f == 1:
                d = v - a
            elif f == 2:
                d = v - b
            elif f == 3:
                d = v - ((a + b) >> 1)
            else:
                d = v - paeth(a, b, c)
            enc.append(d & 0xFF)
        out.append(f)
        out += enc
        prev = row
    return bytes(out)


def stream(dictionary, data):
    return dictionary + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def classic_pdf(path, objs, root=1, trailer_extra=b""):
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for num in sorted(objs):
        offsets[num] = len(out)
        out += b"%d 0 obj\n" % num + objs[num] + b"\nendobj\n"
    xref_off = len(out)
    maxnum = max(objs)
    out += b"xref\n0 %d\n" % (maxnum + 1)
    out += b"0000000000 65535 f \n"
    for num in range(1, maxnum + 1):
        if num in offsets:
            out += b"%010d 00000 n \n" % offsets[num]
        else:
            out += b"0000000000 65535 f \n"
    out += b"trailer\n<< /Size %d /Root %d 0 R " % (maxnum + 1, root)
    out += trailer_extra + b">>\nstartxref\n%d\n%%%%EOF\n" % xref_off
    with open(path, "wb") as f:
        f.write(out)


def dct_image_obj(jpeg, w, h, colors="DeviceRGB"):
    return stream(
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s "
        b"/BitsPerComponent 8 /Filter /DCTDecode" % (w, h, colors.encode()),
        jpeg)


def flate_image_obj(w, h, colorspace, bpc, data, predictor, colors):
    return stream(
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s "
        b"/BitsPerComponent %d /Filter /FlateDecode /DecodeParms << /Predictor %d "
        b"/Columns %d /Colors %d /BitsPerComponent %d >>"
        % (w, h, colorspace.encode(), bpc, predictor, w, colors, bpc),
        data)


def main():
    out_dir = sys.argv[1]
    jpeg_path = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    jpeg = open(jpeg_path, "rb").read()
    jw, jh = jpeg_dims(jpeg)

    # ── 1. 经典 xref + 单页 DCT ─────────────────────────────────────────────
    content = b"q 612 0 0 792 0 0 cm /Im0 Do Q\n"
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 4 0 R >> >> "
           b"/Contents 5 0 R >>",
        4: dct_image_obj(jpeg, jw, jh),
        5: stream(b"<<", content),
    }
    classic_pdf(os.path.join(out_dir, "classic.pdf"), objs)

    # ── 2. xref stream + ObjStm ────────────────────────────────────────────
    gw, gh = 40, 30
    gray_rows = [bytes((x * 6 + y * 2) & 0xFF for x in range(gw)) for y in range(gh)]
    gray_png = png_encode(gray_rows, 1, [0, 1, 2, 3, 4])
    gray_zip = zlib.compress(gray_png)
    img = flate_image_obj(gw, gh, "DeviceGray", 8, gray_zip, 15, 1)
    cont = b"q 300 0 0 400 0 0 cm /Im0 Do Q\n"

    cat = b"<< /Type /Catalog /Pages 2 0 R >>"
    pages = b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 300 400] >>"
    page = (b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 5 0 R >> >> "
            b"/Contents 6 0 R >>")
    body = cat + b"\n" + pages + b"\n" + page
    o2 = len(cat) + 1
    o3 = o2 + len(pages) + 1
    header = b"1 0 2 %d 3 %d " % (o2, o3)
    first = len(header)
    objstm_zip = zlib.compress(header + body)

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}

    def put(num, data):
        offsets[num] = len(out)
        out.extend(b"%d 0 obj\n" % num)
        out.extend(data)
        out.extend(b"\nendobj\n")

    put(4, stream(b"<< /Type /ObjStm /N 3 /First %d /Filter /FlateDecode" % first, objstm_zip))
    put(5, img)
    put(6, stream(b"<<", cont))
    xref_off = len(out)
    rows = [bytes([0, 0, 0, 0])]
    for idx in range(3):
        rows.append(bytes([2]) + (4).to_bytes(2, "big") + bytes([0]))
    for num in (4, 5, 6):
        rows.append(bytes([1]) + offsets[num].to_bytes(2, "big") + bytes([0]))
    rows.append(bytes([1]) + xref_off.to_bytes(2, "big") + bytes([0]))
    prev = bytes(4)
    enc = bytearray()
    for row in rows:
        enc.append(2)
        enc.extend((row[i] - prev[i]) & 0xFF for i in range(4))
        prev = row
    xz = zlib.compress(bytes(enc))
    xdict = (b"<< /Type /XRef /Size 8 /Root 1 0 R /W [1 2 1] /Index [0 8] "
             b"/Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 4 >>")
    out.extend(b"7 0 obj\n")
    out.extend(xdict + b" /Length %d >>\nstream\n" % len(xz))
    out.extend(xz)
    out.extend(b"\nendstream\nendobj\n")
    out.extend(b"startxref\n%d\n%%%%EOF\n" % xref_off)
    with open(os.path.join(out_dir, "xrefstm.pdf"), "wb") as f:
        f.write(out)

    # ── 3. Flate 灰度 + PNG predictor 15（filter 轮换）─────────────────────
    fw, fh = 64, 48
    rows = [bytes((x * 4 + y * 3) & 0xFF for x in range(fw)) for y in range(fh)]
    png = png_encode(rows, 1, [0, 1, 2, 3, 4])
    data = zlib.compress(png)
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 400 300] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 4 0 R >> >> "
           b"/Contents 5 0 R >>",
        4: flate_image_obj(fw, fh, "DeviceGray", 8, data, 15, 1),
        5: stream(b"<<", b"q 400 0 0 300 0 0 cm /Im0 Do Q\n"),
    }
    classic_pdf(os.path.join(out_dir, "flate_gray.pdf"), objs)

    # ── 4. Flate RGB + predictor 12 ─────────────────────────────────────────
    rw, rh = 32, 24
    rgb_rows = [bytes((x * 8 + y) & 0xFF for x in range(rw * 3)) for y in range(rh)]
    rgb_png = png_encode(rgb_rows, 3, [2])
    rgb_zip = zlib.compress(rgb_png)
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 320 240] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 4 0 R >> >> "
           b"/Contents 5 0 R >>",
        4: flate_image_obj(rw, rh, "DeviceRGB", 8, rgb_zip, 12, 3),
        5: stream(b"<<", b"q 320 0 0 240 0 0 cm /Im0 Do Q\n"),
    }
    classic_pdf(os.path.join(out_dir, "flate_rgb.pdf"), objs)

    # ── 5. /Rotate 90 ──────────────────────────────────────────────────────
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Rotate 90 /Resources << /XObject "
           b"<< /Im0 4 0 R >> >> /Contents 5 0 R >>",
        4: dct_image_obj(jpeg, jw, jh),
        5: stream(b"<<", content),
    }
    classic_pdf(os.path.join(out_dir, "rotate90.pdf"), objs)

    # ── 6. 多图页（大图 + 小 logo）──────────────────────────────────────────
    logo_rows = [bytes([255]) * 16 for _ in range(16)]
    logo_zip = zlib.compress(png_encode(logo_rows, 1, [0]))
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 4 0 R "
           b"/Im1 5 0 R >> >> /Contents 6 0 R >>",
        4: dct_image_obj(jpeg, jw, jh),
        5: flate_image_obj(16, 16, "DeviceGray", 8, logo_zip, 1, 1),
        6: stream(b"<<", b"q 612 0 0 792 0 0 cm /Im0 Do Q\n"
                         b"q 16 0 0 16 10 10 cm /Im1 Do Q\n"),
    }
    classic_pdf(os.path.join(out_dir, "multi.pdf"), objs)

    # ── 7. 纯文字页 ────────────────────────────────────────────────────────
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>",
        4: stream(b"<<", b"BT /F1 12 Tf 72 700 Td (hello) Tj ET\n"),
    }
    classic_pdf(os.path.join(out_dir, "textonly.pdf"), objs)

    # ── 8. 不支持的滤镜（JPX）────────────────────────────────────────────
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 4 0 R >> >> "
           b"/Contents 5 0 R >>",
        4: stream(b"<< /Type /XObject /Subtype /Image /Width 100 /Height 100 "
                  b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /JPXDecode",
                  b"\x00\x00\x00\x0cjP  \r\n\x87\n"),
        5: stream(b"<<", content),
    }
    classic_pdf(os.path.join(out_dir, "unsupported.pdf"), objs)

    # ── 9. 加密 / 坏 xref / 截断 / 尺寸炸弹 ────────────────────────────────
    # 加密样本需要 /Encrypt 指向存在的对象。
    enc_objs = dict(objs)
    enc_objs[6] = b"<< /Filter /Standard /V 1 /R 2 /O <00> /U <00> /P -4 >>"
    classic_pdf(os.path.join(out_dir, "encrypted.pdf"), enc_objs,
                trailer_extra=b"/Encrypt 6 0 R ")

    with open(os.path.join(out_dir, "badxref.pdf"), "wb") as f:
        f.write(b"%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n"
                b"startxref\n999999\n%%EOF\n")

    classic = open(os.path.join(out_dir, "classic.pdf"), "rb").read()
    with open(os.path.join(out_dir, "truncated.pdf"), "wb") as f:
        f.write(classic[:len(classic) // 2])

    bomb_data = zlib.compress(b"\x00" * 64)
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 4 0 R >> >> "
           b"/Contents 5 0 R >>",
        4: flate_image_obj(100000, 100000, "DeviceGray", 8, bomb_data, 1, 1),
        5: stream(b"<<", content),
    }
    classic_pdf(os.path.join(out_dir, "bomb.pdf"), objs)


if __name__ == "__main__":
    main()
