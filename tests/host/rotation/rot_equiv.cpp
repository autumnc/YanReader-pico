// 主机端等价性对拍：main/gfx/fb_fast.h 的旋转映射（P4 收敛后唯一一份）与它取代的
// 旧代码逐像素等价、且 to/from 互为逆。纯整数运算，不需要 epdiy —— 这正是 P4 那
// 条"动推屏路径前必须先有主机端对拍"要求的落地。
//
// 对拍的三件事：
//   A. fb_rot_to_phys   == 旧 fb_fast.h 里手抄的 to-phys switch（逐逻辑像素）
//   B. fb_rot_from_phys == 旧 ui_render.cpp diff_bounding_rect 里手抄的 from-phys switch（逐物理像素）
//   C. fb_rot_from_phys(fb_rot_to_phys(p)) == p（四种旋转、满屏逐像素的往返）
//
// 编译运行：tests/host/rotation/run.sh（不需要 IDF 环境）。

#include <cstdio>
#include <cstdlib>

// 与 epdiy.h 的 enum EpdRotation 同值（这里不引 epdiy，保持纯主机可编译）。
enum { EPD_ROT_LANDSCAPE = 0, EPD_ROT_PORTRAIT = 1,
       EPD_ROT_INVERTED_LANDSCAPE = 2, EPD_ROT_INVERTED_PORTRAIT = 3 };

// ── 新代码：main/gfx/fb_fast.h 里那两份（原样抄来，去掉 inline 存储类）───
static void fb_rot_to_phys(int rot, int w, int h, int x, int y, int *px, int *py) {
    int ax = x, ay = y;
    switch (rot) {
        case EPD_ROT_LANDSCAPE:
            break;
        case EPD_ROT_PORTRAIT: {
            int t = ax; ax = ay; ay = t;
            ax = w - ax - 1;
            break;
        }
        case EPD_ROT_INVERTED_LANDSCAPE:
            ax = w - ax - 1;
            ay = h - ay - 1;
            break;
        case EPD_ROT_INVERTED_PORTRAIT: {
            int t = ax; ax = ay; ay = t;
            ay = h - ay - 1;
            break;
        }
        default:
            break;
    }
    *px = ax; *py = ay;
}
static void fb_rot_from_phys(int rot, int w, int h, int px, int py, int *lx, int *ly) {
    switch (rot) {
        case EPD_ROT_LANDSCAPE:          *lx = px;         *ly = py;         break;
        case EPD_ROT_PORTRAIT:           *lx = py;         *ly = w - 1 - px; break;
        case EPD_ROT_INVERTED_LANDSCAPE: *lx = w - 1 - px; *ly = h - 1 - py; break;
        default:                         *lx = h - 1 - py; *ly = px;         break;
    }
}

// ── 旧代码 ───────────────────────────────────────────────────────────────
// 旧 fb_fast.h fb_fast_to_phys 的 switch 体（去掉越界返回）。
static void old_to_phys(int rot, int w, int h, int x, int y, int *px, int *py) {
    int ax = x, ay = y;
    switch (rot) {
        case EPD_ROT_LANDSCAPE: break;
        case EPD_ROT_PORTRAIT: { int t = ax; ax = ay; ay = t; ax = w - ax - 1; break; }
        case EPD_ROT_INVERTED_LANDSCAPE: ax = w - ax - 1; ay = h - ay - 1; break;
        case EPD_ROT_INVERTED_PORTRAIT: { int t = ax; ax = ay; ay = t; ay = h - ay - 1; break; }
        default: break;
    }
    *px = ax; *py = ay;
}
// 旧 ui_render.cpp diff_bounding_rect 的 add() 里的映射体。
static void old_from_phys(int rot, int fb_w, int fb_h, int px, int py, int *lx, int *ly) {
    switch (rot) {
        case EPD_ROT_LANDSCAPE:          *lx = px;             *ly = py; break;
        case EPD_ROT_PORTRAIT:           *lx = py;             *ly = fb_w - 1 - px; break;
        case EPD_ROT_INVERTED_LANDSCAPE: *lx = fb_w - 1 - px;  *ly = fb_h - 1 - py; break;
        default:                         *lx = fb_h - 1 - py;  *ly = px; break;
    }
}

static int fails = 0;
static void chk(bool ok, const char *what, int rot, int a, int b) {
    if (!ok && ++fails <= 10)
        printf("  FAIL %s rot=%d at (%d,%d)\n", what, rot, a, b);
}

int main(void) {
    const int ROTS[4] = {EPD_ROT_LANDSCAPE, EPD_ROT_PORTRAIT,
                         EPD_ROT_INVERTED_LANDSCAPE, EPD_ROT_INVERTED_PORTRAIT};
    // 实机的两套物理尺寸（board.cpp：写作/计划横屏 1216×684，阅读竖屏 684×1216）。
    const int WH[][2] = {{1216, 684}, {684, 1216}};

    long checked = 0;
    for (int r = 0; r < 4; r++) {
        const int rot = ROTS[r];
        for (int k = 0; k < 2; k++) {
            const int w = WH[k][0], h = WH[k][1];
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int px, py, qx, qy, lx, ly;

                    // A: 新 to_phys == 旧 to_phys
                    fb_rot_to_phys(rot, w, h, x, y, &px, &py);
                    old_to_phys(rot, w, h, x, y, &qx, &qy);
                    chk(px == qx && py == qy, "to_phys", rot, x, y);

                    // C: from(to(p)) == p（只有在物理缓冲范围内的逻辑点才有意义；
                    //    但 to_phys 是满射且 to 后仍在 [0,w)x[0,h)，故恒成立）
                    fb_rot_from_phys(rot, w, h, px, py, &lx, &ly);
                    chk(lx == x && ly == y, "roundtrip", rot, x, y);

                    // B: 新 from_phys == 旧 from_phys（逐物理点；这里物理点取 to 的像）
                    int ox, oy;
                    old_from_phys(rot, w, h, px, py, &ox, &oy);
                    chk(lx == ox && ly == oy, "from_phys", rot, px, py);
                    checked++;
                }
            }
        }
    }
    // 再对"未知 rot"（default 分支）单独比一次：ui_render 旧代码里 default=270°，
    // fb_fast 旧代码里 default=恒等 —— 新的两份各随各的旧语义。这里只验 from 的 default。
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int a, b, c, d;
            fb_rot_from_phys(9, 8, 8, x, y, &a, &b);
            old_from_phys(9, 8, 8, x, y, &c, &d);
            chk(a == c && b == d, "from_phys-default", 9, x, y);
        }

    if (fails == 0) printf("OK  rotation mapping equivalent (%ld points checked)\n", checked);
    else            printf("FAILED  %d mismatches\n", fails);
    return fails ? 1 : 0;
}
