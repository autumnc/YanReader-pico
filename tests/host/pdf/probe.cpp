// Host-side probe for the minimal PDF reader (components/crossmux/lib/Pdf).
// Plain g++ + the vendored miniz; no ESP-IDF, no device, no third-party PDF lib.
//
//   tests/host/pdf/run.sh            build fixtures, run, diff vs expected.txt
//   tests/host/pdf/run.sh --update   regenerate expected.txt

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <PdfContent.h>
#include <PdfDocument.h>
#include <PdfFilters.h>
#include <PdfImage.h>
#include <PdfPage.h>
#include <PdfPxc.h>
#include <PdfViewport.h>
#include <PdfXref.h>

namespace {

int g_fail = 0;

#define CHECK(cond, msg)                          \
  do {                                            \
    if (cond) {                                   \
      printf("ok   %s\n", msg);                   \
    } else {                                      \
      printf("FAIL %s\n", msg);                   \
      g_fail++;                                   \
    }                                             \
  } while (0)

uint32_t crc32(const uint8_t *data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

struct FileSource final : PdfSource {
  std::vector<uint8_t> data;
  bool load(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
      fclose(f);
      return false;
    }
    data.resize(static_cast<size_t>(n));
    const size_t got = fread(data.data(), 1, data.size(), f);
    fclose(f);
    return got == data.size();
  }
  uint64_t size() const override { return data.size(); }
  int readAt(uint64_t off, void *buf, size_t len) const override {
    if (off > data.size() || len > data.size() - static_cast<size_t>(off)) return -1;
    memcpy(buf, data.data() + off, len);
    return static_cast<int>(len);
  }
};

struct MemSink final : PdfPxcSink {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bytes;
  bool begin(int w, int h) {
    width = w;
    height = h;
    bytes.assign(static_cast<size_t>(pdfPxcRowBytes(w)) * h, 0);
    return true;
  }
  bool writeRow(int y, const uint8_t *row, int n) override {
    if (y < 0 || y >= height || n != pdfPxcRowBytes(width)) return false;
    memcpy(bytes.data() + static_cast<size_t>(y) * n, row, static_cast<size_t>(n));
    return true;
  }
  uint8_t level(int x, int y) const {
    return pdfPxcGet(bytes.data() + static_cast<size_t>(y) * pdfPxcRowBytes(width), x);
  }
};

std::vector<uint8_t> makePxc(int w, int h, int inkX0, int inkY0, int inkX1, int inkY1) {
  std::vector<uint8_t> out(static_cast<size_t>(kPdfPxcHeaderBytes + pdfPxcRowBytes(w) * h), 0);
  out[0] = static_cast<uint8_t>(w & 0xFF);
  out[1] = static_cast<uint8_t>(w >> 8);
  out[2] = static_cast<uint8_t>(h & 0xFF);
  out[3] = static_cast<uint8_t>(h >> 8);
  out[4] = kPdfPxcVersion;
  for (int y = 0; y < h; y++) {
    uint8_t *row = out.data() + kPdfPxcHeaderBytes + static_cast<size_t>(y) * pdfPxcRowBytes(w);
    for (int x = 0; x < w; x++) {
      const bool ink = x >= inkX0 && x < inkX1 && y >= inkY0 && y < inkY1;
      pdfPxcSet(row, x, ink ? 3 : 15);
    }
  }
  return out;
}

std::string pathOf(const std::string &dir, const char *name) { return dir + "/" + name; }

void testClassic(const std::string &dir) {
  FileSource fs;
  CHECK(fs.load(pathOf(dir, "classic.pdf")), "fixture classic loads");
  PdfDocument doc;
  PdfError e = PdfError::Ok;
  std::string m;
  const bool opened = doc.open(fs, &e, &m);
  CHECK(opened, "classic: open");
  if (!opened) printf("classic open err=%s msg=%s\n", pdfErrorText(e), m.c_str());
  CHECK(doc.pageCount() == 1, "classic: one page");
  if (doc.pageCount() < 1 || !doc.pageDict(0)) return;

  PdfPageGeom geom;
  CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "classic: page geometry");
  printf("classic geom media=%.0fx%.0f crop=%.0fx%.0f rot=%d\n", geom.mediaBox.w(),
         geom.mediaBox.h(), geom.cropBox.w(), geom.cropBox.h(), geom.rotate);

  PdfPageImage pi;
  CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "classic: main image found");
  printf("classic coverage=%.2f\n", pi.coverage);

  PdfImageInfo info;
  CHECK(pdfImageInfo(doc, pi.imageDict, &info, &e, &m), "classic: image info");
  printf("classic image=%dx%d dct=%d comps=%d\n", info.width, info.height, info.isDct,
         info.components);

  std::vector<uint8_t> jpg;
  CHECK(pdfCopyDctStream(doc, pi.imageDict, &jpg, &e, &m), "classic: DCT extract");
  printf("classic dct bytes=%zu crc=%08x\n", jpg.size(), crc32(jpg.data(), jpg.size()));
}

void testXrefStream(const std::string &dir) {
  FileSource fs;
  CHECK(fs.load(pathOf(dir, "xrefstm.pdf")), "fixture xrefstm loads");
  PdfDocument doc;
  PdfError e = PdfError::Ok;
  std::string m;
  CHECK(doc.open(fs, &e, &m), "xrefstm: open");
  printf("xrefstm pages=%d\n", doc.pageCount());
  CHECK(doc.pageCount() == 1, "xrefstm: one page (catalog in ObjStm)");
  if (!doc.pageDict(0)) return;

  PdfPageGeom geom;
  CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "xrefstm: geometry");
  PdfPageImage pi;
  CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "xrefstm: main image");
  PdfImageInfo info;
  CHECK(pdfImageInfo(doc, pi.imageDict, &info, &e, &m), "xrefstm: image info");
  printf("xrefstm image=%dx%d comps=%d\n", info.width, info.height, info.components);

  MemSink sink;
  sink.begin(info.width, info.height);
  CHECK(pdfDecodeImageToPxc(doc, pi.imageDict, sink, &info, nullptr, nullptr, &e, &m),
        "xrefstm: flate decode");
  printf("xrefstm pxc crc=%08x s00=%d s2020=%d\n", crc32(sink.bytes.data(), sink.bytes.size()),
         sink.level(0, 0), sink.level(20, 20));
}

void testFlateGray(const std::string &dir) {
  FileSource fs;
  CHECK(fs.load(pathOf(dir, "flate_gray.pdf")), "fixture flate_gray loads");
  PdfDocument doc;
  PdfError e = PdfError::Ok;
  std::string m;
  CHECK(doc.open(fs, &e, &m), "flate_gray: open");
  if (!doc.pageDict(0)) return;
  PdfPageGeom geom;
  CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "flate_gray: geometry");
  PdfPageImage pi;
  CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "flate_gray: main image");
  PdfImageInfo info;
  CHECK(pdfImageInfo(doc, pi.imageDict, &info, &e, &m), "flate_gray: image info");
  MemSink sink;
  sink.begin(info.width, info.height);
  CHECK(pdfDecodeImageToPxc(doc, pi.imageDict, sink, &info, nullptr, nullptr, &e, &m),
        "flate_gray: decode with PNG predictor");
  printf("flate_gray pxc=%dx%d crc=%08x s00=%d s1020=%d s6347=%d\n", info.width, info.height,
         crc32(sink.bytes.data(), sink.bytes.size()), sink.level(0, 0), sink.level(10, 20),
         sink.level(63, 47));
}

void testFlateRgb(const std::string &dir) {
  FileSource fs;
  CHECK(fs.load(pathOf(dir, "flate_rgb.pdf")), "fixture flate_rgb loads");
  PdfDocument doc;
  PdfError e = PdfError::Ok;
  std::string m;
  CHECK(doc.open(fs, &e, &m), "flate_rgb: open");
  if (!doc.pageDict(0)) return;
  PdfPageGeom geom;
  CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "flate_rgb: geometry");
  PdfPageImage pi;
  CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "flate_rgb: main image");
  PdfImageInfo info;
  CHECK(pdfImageInfo(doc, pi.imageDict, &info, &e, &m), "flate_rgb: image info");
  CHECK(info.components == 3, "flate_rgb: 3 components");
  MemSink sink;
  sink.begin(info.width, info.height);
  CHECK(pdfDecodeImageToPxc(doc, pi.imageDict, sink, &info, nullptr, nullptr, &e, &m),
        "flate_rgb: decode with Up predictor");
  printf("flate_rgb pxc=%dx%d crc=%08x s00=%d s1512=%d\n", info.width, info.height,
         crc32(sink.bytes.data(), sink.bytes.size()), sink.level(0, 0), sink.level(15, 12));
}

void testSelectAndErrors(const std::string &dir) {
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "multi.pdf")), "fixture multi loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    CHECK(doc.open(fs, &e, &m), "multi: open");
    if (!doc.pageDict(0)) return;
    PdfPageGeom geom;
    CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "multi: geometry");
    PdfPageImage pi;
    CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "multi: main image");
    PdfImageInfo info;
    CHECK(pdfImageInfo(doc, pi.imageDict, &info, &e, &m), "multi: image info");
    printf("multi coverage=%.2f image=%dx%d\n", pi.coverage, info.width, info.height);
    CHECK(info.width > 16, "multi: picks the large image");
  }
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "textonly.pdf")), "fixture textonly loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    CHECK(doc.open(fs, &e, &m), "textonly: open");
    if (!doc.pageDict(0)) return;
    PdfPageGeom geom;
    CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "textonly: geometry");
    PdfPageImage pi;
    const bool ok = pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m);
    CHECK(!ok && e == PdfError::NoPageImage, "textonly: NoPageImage");
    printf("textonly err=%s\n", pdfErrorText(e));
  }
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "unsupported.pdf")), "fixture unsupported loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    CHECK(doc.open(fs, &e, &m), "unsupported: open");
    if (!doc.pageDict(0)) return;
    PdfPageGeom geom;
    CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "unsupported: geometry");
    PdfPageImage pi;
    CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "unsupported: main image");
    PdfImageInfo info;
    const bool ok = pdfImageInfo(doc, pi.imageDict, &info, &e, &m);
    CHECK(!ok && e == PdfError::UnsupportedFilter, "unsupported: UnsupportedFilter");
    printf("unsupported err=%s\n", pdfErrorText(e));
  }
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "encrypted.pdf")), "fixture encrypted loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    const bool ok = doc.open(fs, &e, &m);
    CHECK(!ok && e == PdfError::Encrypted, "encrypted: Encrypted");
    printf("encrypted err=%s\n", pdfErrorText(e));
  }
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "badxref.pdf")), "fixture badxref loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    CHECK(!doc.open(fs, &e, &m), "badxref: rejected");
    printf("badxref err=%s\n", pdfErrorText(e));
  }
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "truncated.pdf")), "fixture truncated loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    CHECK(!doc.open(fs, &e, &m), "truncated: rejected");
    printf("truncated err=%s\n", pdfErrorText(e));
  }
  {
    FileSource fs;
    CHECK(fs.load(pathOf(dir, "bomb.pdf")), "fixture bomb loads");
    PdfDocument doc;
    PdfError e = PdfError::Ok;
    std::string m;
    CHECK(doc.open(fs, &e, &m), "bomb: open");
    if (!doc.pageDict(0)) return;
    PdfPageGeom geom;
    CHECK(pdfReadPageGeom(*doc.pageDict(0), &geom, &e, &m), "bomb: geometry");
    PdfPageImage pi;
    CHECK(pdfFindPageImage(doc, *doc.pageDict(0), geom, &pi, &e, &m), "bomb: main image");
    PdfImageInfo info;
    const bool ok = pdfImageInfo(doc, pi.imageDict, &info, &e, &m);
    CHECK(!ok && e == PdfError::TooLarge, "bomb: TooLarge");
    printf("bomb err=%s\n", pdfErrorText(e));
  }
}

void testPxcCrop() {
  const std::vector<uint8_t> pxc = makePxc(200, 100, 40, 20, 160, 80);
  PdfMemSource src(pxc.data(), pxc.size());
  PdfPxcHeader hdr;
  PdfError e = PdfError::Ok;
  std::string m;
  CHECK(pdfPxcReadHeader(src, &hdr, &e, &m), "pxc: header");
  CHECK(hdr.width == 200 && hdr.height == 100, "pxc: dimensions");
  PdfPxcCropBox crop;
  CHECK(pdfPxcAutoCrop(src, hdr, &crop, &e, &m), "pxc: auto crop");
  CHECK(crop.valid, "pxc: crop valid");
  printf("pxc crop=%d,%d,%d,%d\n", crop.x0, crop.y0, crop.x1, crop.y1);
  CHECK(crop.x0 == 30 && crop.y0 == 15 && crop.x1 == 170 && crop.y1 == 85,
        "pxc: crop bbox (15% cap)");
}

void testViewport() {
  PdfViewSpec spec;
  spec.srcW = 200;
  spec.srcH = 100;
  spec.areaX = 0;
  spec.areaY = 0;
  spec.areaW = 600;
  spec.areaH = 800;
  spec.zoomMode = kPdfZoomFitPage;

  PdfViewMap map;
  PdfError e = PdfError::Ok;
  std::string m;
  CHECK(pdfComputeView(spec, &map, &e, &m), "view: fit page");
  printf("view fitpage scale=%.1f disp=%dx%d off=%.1f,%.1f\n", map.scale, map.dispW, map.dispH,
         map.offX, map.offY);
  CHECK(std::fabs(map.scale - 3.0) < 1e-9, "view: fit page scale 3");
  int sx = -1, sy = -1;
  CHECK(map.screenToSource(0.1, 250.1, &sx, &sy) && sx == 0 && sy == 0, "view: top-left maps");

  spec.zoomMode = kPdfZoomFitWidth;
  CHECK(pdfComputeView(spec, &map, &e, &m), "view: fit width");
  printf("view fitwidth scale=%.1f off=%.1f,%.1f\n", map.scale, map.offX, map.offY);

  spec.zoomMode = kPdfZoomOriginal;
  CHECK(pdfComputeView(spec, &map, &e, &m), "view: original");
  printf("view original scale=%.1f off=%.1f,%.1f\n", map.scale, map.offX, map.offY);

  // 旋转：每个角度下源像素中心 → 屏幕 → 源，必须回到同一个像素。
  for (int rot : {0, 90, 180, 270}) {
    spec.zoomMode = kPdfZoomFitPage;
    spec.rotate = rot;
    CHECK(pdfComputeView(spec, &map, &e, &m), "view: rotate map");
    const int px = 37, py = 61;
    double u, v, sxp, syp;
    map.sourceToDisplay(px, py, &u, &v);
    map.sourceToScreen(px, py, &sxp, &syp);
    int bx = -1, by = -1;
    const bool inside = map.screenToSource(sxp, syp, &bx, &by);
    CHECK(inside && bx == px && by == py, "view: rotate round trip");
    printf("view rot=%d src=%d,%d disp=%.1f,%.1f screen=%.1f,%.1f\n", rot, px, py, u, v, sxp,
           syp);
  }

  // 裁边视图
  spec.rotate = 0;
  spec.crop = true;
  spec.cropBox.valid = true;
  spec.cropBox.x0 = 40;
  spec.cropBox.y0 = 20;
  spec.cropBox.x1 = 160;
  spec.cropBox.y1 = 80;
  CHECK(pdfComputeView(spec, &map, &e, &m), "view: crop");
  printf("view crop scale=%.1f disp=%dx%d off=%.1f,%.1f\n", map.scale, map.dispW, map.dispH,
         map.offX, map.offY);
  CHECK(map.screenToSource(0.1, 250.1, &sx, &sy) && sx == 40 && sy == 20, "view: crop origin");

  // 双击锚点：放大后该源像素仍在按下的屏幕位置。
  spec.crop = false;
  spec.zoomMode = kPdfZoomFitPage;
  spec.zoom = 1.0;
  CHECK(pdfComputeView(spec, &map, &e, &m), "view: anchor base");
  const int ax = 120, ay = 40;
  double tapX, tapY;
  map.sourceToScreen(ax, ay, &tapX, &tapY);
  spec.zoom = 4.0;
  CHECK(pdfComputeView(spec, &map, &e, &m), "view: anchor zoomed");
  double panX = 0, panY = 0;
  pdfPanForAnchor(map, tapX, tapY, ax, ay, &panX, &panY);
  map.offX = (map.areaW - map.dispW * map.scale) * 0.5 + panX;
  map.offY = (map.areaH - map.dispH * map.scale) * 0.5 + panY;
  double nowX, nowY;
  map.sourceToScreen(ax, ay, &nowX, &nowY);
  printf("view anchor tap=%.1f,%.1f pan=%.1f,%.1f now=%.1f,%.1f\n", tapX, tapY, panX, panY,
         nowX, nowY);
  CHECK(std::fabs(nowX - tapX) < 0.01 && std::fabs(nowY - tapY) < 0.01,
        "view: double-tap anchor holds");
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];

  testClassic(dir);
  testXrefStream(dir);
  testFlateGray(dir);
  testFlateRgb(dir);
  testSelectAndErrors(dir);
  testPxcCrop();
  testViewport();

  printf("failures=%d\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
