#!/usr/bin/env python3
"""合成 probe 用的渐进式 JPEG 素材，原始 PPM 写到 stdout。

    python3 mkfixture.py | cjpeg -progressive -quality 80 -outfile fixture.jpg

内容刻意混了三种频率，因为要抓的那个 bug（只留低频、丢掉折叠项）专挑高频发作：

  · 大块平滑渐变 + 一段斜向明暗过渡   —— 低频，折叠项没丢时两者都解得对，抓不出问题；
  · 硬边矩形/圆盘、文字状细黑笔画     —— 中频，丢了折叠项表现为"边缘发糊"；
  · 棋盘格细纹理带 + 细斜线           —— 高频，丢了折叠项这里差得最狠（实测 >5 级）。

尺寸取 1200×1600（与仓里那几本渐进式封面一致），**都是 16 的整数倍**，这样块边界
正好落在图像边界上，不会掺进"最后一个块行跨了编码器补的边"那种边缘差异。
"""
import sys

W, H = 1200, 1600


def rnd(x, y, salt=0):
    """确定性伪随机（LCG），不依赖 random 的实现版本。"""
    v = (x * 374761393 + y * 668265263 + salt * 2147483647) & 0xFFFFFFFF
    v = (v ^ (v >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((v ^ (v >> 16)) & 0xFFFF) / 65535.0


def main():
    out = sys.stdout.buffer
    out.write(b"P6\n%d %d\n255\n" % (W, H))
    row = bytearray(W * 3)

    # 圆盘：中频硬边，放几个
    discs = [(300, 350, 180), (900, 500, 240), (500, 1150, 300), (1000, 1350, 130)]

    for y in range(H):
        i = 0
        for x in range(W):
            # 低频：斜向渐变 + 竖向缓变
            r = int(40 + 120.0 * (x + y) / (W + H) + 30.0 * ((y / H) ** 2))
            g = int(60 + 90.0 * (y / H) + 40.0 * (x / W))
            b = int(180 - 110.0 * (x / W))

            # 中频：硬边圆盘（内部提亮，边界一圈压暗做振铃）
            for cx, cy, rad in discs:
                d2 = (x - cx) ** 2 + (y - cy) ** 2
                if d2 < rad * rad:
                    r += 55
                    g += 45
                    b -= 40
                    if d2 > (rad - 6) ** 2:
                        r -= 60
                        g -= 60
                        b -= 60

            # 中频：文字状细黑笔画（等宽竖条 + 横条），模拟封面上的书名
            if 120 < y < 260:
                if (x % 46) < 26 and 200 < x < 1000:
                    r = g = b = 18
            if 880 < y < 960 and (x // 30) % 3 == 0 and 120 < x < 1080:
                r = g = b = 25

            # 高频：棋盘格细纹理带（周期 2px，最接近奈奎斯特）
            if 1050 < y < 1250:
                if ((x >> 1) + (y >> 1)) & 1:
                    r += 70
                    g += 70
                    b += 70
                else:
                    r -= 70
                    g -= 70
                    b -= 70

            # 高频：细斜线
            if 1320 < y < 1520:
                if (x + y) % 7 == 0:
                    r = g = b = 245

            # 一点点确定性的颗粒，避免整幅过于规整
            n = int((rnd(x, y) - 0.5) * 14)
            r += n
            g += n
            b += n

            row[i] = 0 if r < 0 else (255 if r > 255 else r)
            row[i + 1] = 0 if g < 0 else (255 if g > 255 else g)
            row[i + 2] = 0 if b < 0 else (255 if b > 255 else b)
            i += 3
        out.write(row)


if __name__ == "__main__":
    main()
