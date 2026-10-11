/*
 * 研读 YanReader —— 网页刷机台
 *
 * 浏览器里用 WebSerial + esptool-js 把 Release 的**整包**（引导程序 + 分区表 + 应用）
 * 写到设备 flash 的 0x0。参照 flasher.smn.icu 的交互形状（同一块板子），实现是自己写的，
 * 且有意改了它三处做法 —— 下面每一条都是读 esptool-js 0.7.0 的源码得出的，别照抄回去：
 *
 * 1. 【固件必须同源】不改从 GitHub Release 拉固件。Release 资产（302 跳到
 *    release-assets.githubusercontent.com）**不发 access-control-allow-origin**，
 *    浏览器读不到。所以本站点把 bin 和页面一起放在 GitHub Pages 上（见
 *    scripts/publish_flasher.sh），页面只读同源的 ./firmware.json 与 ./firmware/*.bin。
 *
 * 2. 【长度不能要求 16MiB】本项目的整包是"从 0x0 铺到 app 尾"的 10.27MB，不是整片 16MiB，
 *    所以改成查三处魔数（0x0 引导头 0xE9 / 0x8000 分区表 0xAA 0x50 / 0x10000 应用 0xE9）。
 *    这顺带多出一道闸：能挡下"误把 app-only 的 pjournal_pico.bin 刷到 0x0"这种会砖的操作。
 *
 * 3. 【刷完必须自己复位】writeFlash 走到 compress 那条路时收尾是
 *    flashDeflFinish(false, …)（reboot=false），**芯片留在 stub 里不自启**。所以下面显式调
 *    after("hard_reset")。用库默认那条而不是参考站的 custom_reset "R1|W100|R0"：
 *    HardReset 非 OTG 分支的实现是 `sleep(100); setRTS(false)` —— 只**松开** RTS、不动 DTR；
 *    而那句 custom_reset 是 `RTS 1 → 100ms → RTS 0`，只动 RTS、把 DTR 留在前一档，
 *    正是"USB 串口上给 RTS 脉冲"那个坑的形状（DTR 留错档 → 下次复位进下载模式）。
 *    *实机若发现刷完不重启，第一顺位换 after("soft_reset")，第二顺位才是那句 custom_reset。*
 *
 * 4. 【resetConstructors 一个字都别传】isUsbJtagSerialPort()（VID 0x303A &&
 *    PID == USB_JTAG_SERIAL_PID）命中的设备，constructResetSequence() **会自动**走
 *    UsbJtagSerialReset —— Espressif 给 USB-JTAG 写的官方序列，收尾在 RTS=0,DTR=0 的空闲态。
 *    而 ESPLoader 构造函数里那段合并是**直接赋值**：
 *      if (options.resetConstructors?.usbJTAGSerialReset) {
 *        this.resetConstructors.usbJTAGSerialReset = options.resetConstructors.usbJTAGSerialReset;
 *      }
 *    传 true 会把构造函数替换成布尔值 true，连接时直接崩。**什么都不传才是对的。**
 *
 * 另外两条也是读源码定的：
 *   · calculateMD5Hash 是可选的，不传就不做写后校验（那段 flashMd5sum 会被跳过）。
 *     所以校验由下面显式调 flashMd5sum 完成，比对的是**我们自己算的 md5**（见 md5hex）。
 *     sha256 走现成的 crypto.subtle；md5 只能自己写，理由见文件尾那段。
 *   · baudrate 与 romBaudrate 取相等值，理由写在 loaderOptions.d.ts 里：
 *     "Set equal to baudrate to skip the port reopen entirely, which avoids rebooting boards
 *      whose EN/IO0 lines are disturbed by reopening the port" —— 正是我们想少碰的两条线。
 *     USB-Serial-JTAG 下这个数字不决定吞吐（数据走 USB 批量传输），取保守值即可。
 *
 * 本地文件那条路（页面上「或：用本地 / 其它版本的固件文件」）：
 *   · 就地重跑同一个 gateBytes() + 同一套魔数闸，另加一个可选的"自填校验位"（粘贴 sha256/md5 前几位）。
 *   · 写后校验要拿设备端 flashMd5sum() 吐的 MD5 跟本地比，而 WebCrypto **没有 MD5**
 *     （crypto.subtle 只有 SHA 族），所以下面自己实现了一个 md5hex()。它的正确性由主机端
 *     对拍 md5sum 保证（真机 10.7MB 整包 + app-only 各一份，逐字节比）。
 */

import { Transport, ESPLoader } from "./vendor/esptool-js.bundle.js";

// 全等比较：bundle 里还有个 "ESP32-S31"（另一型号），用 includes("ESP32-S3") 会把它放进来。
const SUPPORTED_CHIP = "ESP32-S3";
const FLASH_MAX_BYTES = 16 * 1024 * 1024;
const BAUD = 115200;

const BOOTLOADER_MAGIC = 0xe9;
const PARTITION_MAGIC0 = 0xaa;
const PARTITION_MAGIC1 = 0x50;
const OFFSET_PARTITION = 0x8000;
const OFFSET_APP = 0x10000;

const $ = (id) => document.getElementById(id);

const el = {
  banner: $("env-banner"),
  fwVersion: $("fw-version"),
  fwTag: $("fw-tag"),
  fwSize: $("fw-size"),
  fwSha: $("fw-sha"),
  fwState: $("fw-state"),
  devInfo: $("dev-info"),
  status: $("status"),
  bar: $("bar"),
  barFill: $("bar-fill"),
  log: $("log"),
  step1: $("step-1"),
  step2: $("step-2"),
  btnConnect: $("btn-connect"),
  btnFlash: $("btn-flash"),
  btnDisconnect: $("btn-disconnect"),
  btnHelp: $("btn-help"),
  btnHelp2: $("btn-help-2"),
  btnHelpClose: $("btn-help-close"),
  help: $("help"),
  fileInput: $("file-input"),
  expectHash: $("expect-hash"),
  fileState: $("file-state"),
  fileKv: $("file-kv"),
  fileName: $("file-name"),
  fileSize: $("file-size"),
  fileSha: $("file-sha"),
  fileMd5: $("file-md5"),
  btnUseOnline: $("btn-use-online"),
};

let manifest = null; // 线上 firmware.json（拉不到也不影响本地文件那条路）
let online = null;   // 线上固件：{source,label,bytes,size,sha256,md5}
let local = null;    // 用户选的文件：同结构
let active = null;   // 当前要刷的那份（online / local 二选一）
let transport = null;
let loader = null;
let port = null;
let connected = false;
let busy = false; // 刷写中：拦关页

// ── 页面小工具 ─────────────────────────────────────────────────────────

let logText = "";

function appendLog(text) {
  logText += text;
  el.log.textContent = logText;
  el.log.scrollTop = el.log.scrollHeight;
}

/** esptool-js 的日志出口。write 可能给半行，所以只追加不换行。 */
const terminal = {
  clean() {
    logText = "";
    el.log.textContent = "";
  },
  writeLine(data) {
    appendLog(data + "\n");
  },
  write(data) {
    appendLog(data);
  },
};

function setStatus(text, kind) {
  el.status.textContent = text || "";
  el.status.className = kind || "";
}

function setProgress(frac) {
  const f = Math.max(0, Math.min(1, Number(frac) || 0));
  el.barFill.style.width = (f * 100).toFixed(1) + "%";
  el.bar.classList.toggle("idle", f <= 0);
}

function showBanner(text) {
  el.banner.textContent = text;
  el.banner.hidden = false;
}

function fmtSize(bytes) {
  return (bytes / 1048576).toFixed(2) + " MB（" + bytes.toLocaleString("en-US") + " 字节）";
}

function refreshButtons() {
  el.btnConnect.disabled = busy || connected || !navigator.serial;
  el.btnFlash.disabled = busy || !connected || !active;
  el.btnDisconnect.hidden = !connected && !loader;
  el.step1.classList.toggle("on", connected);
  el.step2.classList.toggle("on", connected);
}

// ── 固件完整性：sha256 + 三处魔数 ──────────────────────────────────────

async function sha256hex(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  let out = "";
  for (const b of new Uint8Array(digest)) out += b.toString(16).padStart(2, "0");
  return out;
}

// MD5（RFC 1321）—— 只因为 WebCrypto 没有 MD5，而设备端 flashMd5sum() 吐的是 MD5。
// 正确性由主机端对拍 md5sum 保证，别在这儿做优化（正确优先）。
const MD5_S = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];
const MD5_K = new Uint32Array(64);
for (let i = 0; i < 64; i++) MD5_K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);

function md5hex(bytes) {
  const len = bytes.length;
  // 补位到 ≡56 (mod 64)，末尾再放 64 位小端的"比特长度"
  const total = (((len + 8) >> 6) + 1) * 64;
  const buf = new Uint8Array(total);
  buf.set(bytes);
  buf[len] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(total - 8, (len << 3) >>> 0, true);
  dv.setUint32(total - 4, Math.floor(len / 536870912), true); // len*8 的高 32 位

  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < total; off += 64) {
    for (let i = 0; i < 16; i++) M[i] = dv.getUint32(off + i * 4, true);
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16)      { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
      else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
      f = (f + a + MD5_K[i] + M[g]) >>> 0;
      a = d; d = c; c = b;
      const s = MD5_S[i];
      b = (b + (((f << s) | (f >>> (32 - s))) >>> 0)) >>> 0;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }

  const out = new Uint8Array(16);
  const odv = new DataView(out.buffer);
  odv.setUint32(0, a0, true); odv.setUint32(4, b0, true);
  odv.setUint32(8, c0, true); odv.setUint32(12, d0, true);
  let s = "";
  for (const b of out) s += b.toString(16).padStart(2, "0");
  return s;
}

/**
 * 证明手上这个是"从 0x0 开始的整包"，而不是 app-only 的应用镜像。
 * 三条都是合并镜像的结构特征（合并脚本 output 的名字里带 full 但名字不能当证据）。
 */
function checkImageMagic(buf) {
  if (buf.length < OFFSET_APP + 1) {
    throw new Error("固件只有 " + buf.length + " 字节，短得不像整包镜像");
  }
  if (buf[0] !== BOOTLOADER_MAGIC) {
    throw new Error("0x0 处不是引导头（应为 0xE9）—— 不是整包镜像");
  }
  // ★ 这一条才是拦住「误把 app-only 的 pjournal_pico.bin 刷到 0x0」的那道闸：
  //   应用镜像本身也是 0xE9 开头，所以 0x0 那条查不出它，是 0x8000 这处露的馅。
  if (buf[OFFSET_PARTITION] !== PARTITION_MAGIC0 || buf[OFFSET_PARTITION + 1] !== PARTITION_MAGIC1) {
    throw new Error(
      "0x8000 处不是分区表（应为 0xAA 0x50）—— 这看起来是 app-only 的 pjournal_pico.bin，" +
        "刷到 0x0 会让设备起不来。整包文件名形如 yanreader-<版本>-full.bin"
    );
  }
  if (buf[OFFSET_APP] !== BOOTLOADER_MAGIC) {
    throw new Error("0x10000 处不是应用镜像（应为 0xE9）—— 不是整包镜像");
  }
}

/** 长度上限 + checkImageMagic（长度下限与三处魔数都在里面）。线上与本地共用这一份，没有更松的第二条路。 */
function gateBytes(buf) {
  if (buf.length > FLASH_MAX_BYTES) {
    throw new Error("有 " + fmtSize(buf.length) + "，超过 16MB flash 上限");
  }
  checkImageMagic(buf);
}

/**
 * 把一段字节收成"可刷的固件"：过闸 → 算两种摘要 → 可选地与用户填的校验位比对。
 * 线上与本地都走这里，所以两边拿到的保证完全一样。
 */
async function buildFirmware(bytes, source, label, expectHash) {
  gateBytes(bytes);
  const sha256 = await sha256hex(bytes);
  const md5 = md5hex(bytes);

  const want = String(expectHash || "").trim().toLowerCase().replace(/^0x/, "");
  if (want && !sha256.startsWith(want) && !md5.startsWith(want)) {
    throw new Error(
      "校验不符：你填的是 " + want + "，算出来是 sha256 " + sha256.slice(0, 16) +
        "… / md5 " + md5 + " —— 不是这个文件，已中止"
    );
  }
  return { source, label, bytes, size: bytes.length, sha256, md5 };
}

/** 换掉"当前要刷的那份"，并把状态行刷成它的摘要。 */
function setActive(fw) {
  active = fw;
  if (fw) {
    el.fwState.className = "";
    el.fwState.textContent =
      "当前要刷：" + (fw.source === "local" ? "本地文件" : "线上固件") + " · " + fw.label +
      " · " + fmtSize(fw.size) + " · sha256 " + fw.sha256.slice(0, 12) + "… · md5 " + fw.md5.slice(0, 8) + "…";
  }
  el.btnUseOnline.hidden = !(fw && fw.source === "local" && online);
  refreshButtons();
}

async function initFirmware() {
  try {
    const res = await fetch("./firmware.json", { cache: "no-store" });
    if (!res.ok) throw new Error("读固件清单失败（HTTP " + res.status + "）");
    const m = await res.json();

    for (const k of ["version", "tag", "file", "size", "sha256", "md5", "flashOffset"]) {
      if (m[k] === undefined || m[k] === null || m[k] === "") {
        throw new Error("固件清单缺少字段 " + k);
      }
    }
    manifest = m;

    el.fwVersion.textContent = "v" + String(m.version).replace(/^v/i, "");
    el.fwTag.textContent = m.tag + (m.date ? " · " + m.date : "");
    el.fwSize.textContent = fmtSize(Number(m.size));
    el.fwSha.textContent = String(m.sha256).slice(0, 16) + "…";
    el.fwState.textContent = "正在下载线上固件…";

    const fw = await fetch("./" + m.file, { cache: "no-store" });
    if (!fw.ok) throw new Error("下载固件失败（HTTP " + fw.status + "）：" + m.file);
    const buf = new Uint8Array(await fw.arrayBuffer());

    if (buf.length !== Number(m.size)) {
      throw new Error("固件长度不符（清单 " + m.size + "，实收 " + buf.length + "）");
    }

    // 过闸 + 对清单 sha256；顺带我们自己算的 md5 也要和清单对上（两边都算一遍，对不上就中止）
    const built = await buildFirmware(
      buf, "online", "v" + String(m.version).replace(/^v/i, ""), String(m.sha256).toLowerCase()
    );
    if (built.md5 !== String(m.md5).toLowerCase()) {
      throw new Error("md5 自检不符（本地算出 " + built.md5 + "，清单 " + m.md5 + "）—— 已中止");
    }
    online = built;
    if (!local) setActive(online);
  } catch (e) {
    manifest = null;
    online = null;
    if (local) {
      setActive(local);
    } else {
      active = null;
      el.fwState.className = "err";
      el.fwState.textContent =
        "线上固件不可用：" + e.message + " —— 可以展开下面「用本地 / 其它版本的固件文件」，选一个整包 .bin 来刷。";
      refreshButtons();
    }
  }
}

/** 本地文件那条路：读 → 过同一套闸 → 切过去。 */
async function loadLocalFile(file) {
  el.fileState.classList.remove("err");
  el.fileState.textContent = "正在读取 " + file.name + "…";
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const fw = await buildFirmware(bytes, "local", file.name, el.expectHash.value);

    local = fw;
    el.fileName.textContent = file.name;
    el.fileSize.textContent = fmtSize(fw.size);
    el.fileSha.textContent = fw.sha256;
    el.fileMd5.textContent = fw.md5;
    el.fileKv.hidden = false;
    el.fileState.textContent = "✓ 闸都过了，已切到这个文件。";
    setActive(fw);
  } catch (e) {
    local = null;
    el.fileKv.hidden = true;
    el.fileState.classList.add("err");
    el.fileState.textContent = "这个文件不能用：" + e.message;
    if (online) setActive(online);
    else {
      active = null;
      el.fwState.className = "err";
      el.fwState.textContent = "没有可用的固件：线上拉不到，本地这个也不行。";
      refreshButtons();
    }
  }
}

// ── 连接 / 刷写 / 断开 ─────────────────────────────────────────────────

async function releasePort() {
  const t = transport;
  transport = null;
  loader = null;
  port = null;
  connected = false;
  if (t) {
    try {
      await t.disconnect();
    } catch (e) {
      appendLog("> 断开时出错（可忽略）：" + e + "\n");
    }
  }
  refreshButtons();
}

async function connect() {
  setStatus("正在连接…");
  setProgress(0);
  el.btnConnect.disabled = true;
  try {
    if (!navigator.serial) {
      throw new Error("这个浏览器没有 WebSerial，请改用 Chrome 或 Edge（桌面版）");
    }
    port = await navigator.serial.requestPort();

    // 第二参 tracing=false：关掉逐包十六进制转储，日志留给人看的。
    transport = new Transport(port, false);
    loader = new ESPLoader({
      transport,
      baudrate: BAUD,
      romBaudrate: BAUD, // 与 baudrate 相等 → 跳过端口重开（见文件头）
      terminal,
      // ★ 绝不传 resetConstructors —— 见文件头第 4 条
    });

    // main() 默认 default_reset；USB-JTAG-Serial 端口会自动改用官方复位序列。
    const desc = await loader.main();

    // 芯片闸口：不是 ESP32-S3 就地中止，什么都不写。
    if (loader.chip.CHIP_NAME !== SUPPORTED_CHIP) {
      throw new Error(
        "这不是 " + SUPPORTED_CHIP + "（读到 " + loader.chip.CHIP_NAME + "）—— " +
          "本页只支持 Read Pico / 小纸 Pico，已中止，没有写入任何数据"
      );
    }

    connected = true;

    const bits = [desc || SUPPORTED_CHIP];
    try {
      const size = await loader.detectFlashSize();
      if (size) bits.push("flash " + size);
    } catch (_) {
      /* 探测不到就不显示 */
    }
    try {
      const mac = await loader.chip.readMac(loader);
      if (mac && mac.length) {
        bits.push(
          "MAC " +
            Array.from(mac)
              .map((b) => b.toString(16).padStart(2, "0"))
              .join(":")
              .toUpperCase()
        );
      }
    } catch (_) {
      /* 安全下载模式下读不到 MAC */
    }

    el.devInfo.textContent =
      "已连接：" + bits.join(" · ") + "。点下面的按钮开始刷写（写入 → 读回比对 MD5 → 重启）。";
    setStatus("已连接，可以刷写了。", "ok");
  } catch (e) {
    setStatus("连接失败：" + e.message, "err");
    appendLog("> 连接失败：" + (e && e.stack ? e.stack : e) + "\n");
    await releasePort();
  }
  refreshButtons();
}

async function flash() {
  if (!loader || !active || busy) return;

  const offset = (manifest && Number(manifest.flashOffset)) || 0;
  const yes = confirm(
    "即将把 " + active.label + "（" + fmtSize(active.size) + "）写入设备 flash 的 0x" +
      offset.toString(16) + "。\n\n" +
      "sha256 " + active.sha256 + "\nmd5    " + active.md5 + "\n\n" +
      "过程中不要拔线、不要关页面（约 1 分钟）。设备设置与 SD 卡内容不受影响。\n\n继续？"
  );
  if (!yes) return;

  busy = true;
  refreshButtons();
  setProgress(0);
  setStatus("正在写入固件…（请勿拔线）");

  try {
    await loader.writeFlash({
      fileArray: [{ data: active.bytes, address: offset }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false, // 保住 NVS = 保住设备设置与 WiFi 凭据
      compress: true,
      reportProgress: (_fileIndex, written, total) => {
        setProgress(total ? written / total : 0);
      },
    });

    setProgress(1);
    setStatus("写入完成，正在读回 flash 比对 MD5…");

    // writeFlash 不传 calculateMD5Hash 时不做写后校验，所以这里自己比一次。
    const devMd5 = String(await loader.flashMd5sum(offset, active.size)).toLowerCase();
    if (devMd5 !== active.md5) {
      throw new Error(
        "flash 校验不通过（设备 " + devMd5 + " ≠ 本地算出 " + active.md5 + "）—— 重新刷一次试试"
      );
    }
    appendLog("> flash MD5 一致：" + devMd5 + "\n");
    setStatus("校验通过，正在重启设备…");

    // ★ writeFlash 不会自己复位（compress 路径收尾 reboot=false），必须显式来一下。
    await loader.after("hard_reset");

    setStatus("✓ 刷写完成，设备正在重启 —— 屏幕刷新后即可拔线。", "ok");
    setProgress(1);
    await releasePort();
  } catch (e) {
    appendLog("> 刷写失败：" + (e && e.stack ? e.stack : e) + "\n");
    setStatus("刷写失败：" + e.message + "（可以重刷一次；必要时换线换 USB 口）", "err");
  }

  busy = false;
  refreshButtons();
}

// ── 使用帮助浮层 ───────────────────────────────────────────────────────

function openHelp() {
  el.help.hidden = false;
  document.body.style.overflow = "hidden";
  el.help.scrollTop = 0;
}

function closeHelp() {
  el.help.hidden = true;
  document.body.style.overflow = "";
  if (location.hash === "#help") {
    history.replaceState(null, "", location.pathname + location.search);
  }
}

// ── 启动 ───────────────────────────────────────────────────────────────

(function checkEnv() {
  const problems = [];
  if (!window.isSecureContext) {
    problems.push("本页需要通过 https 或 localhost 打开（WebSerial 与固件校验都要求安全上下文）");
  }
  if (!navigator.serial) {
    problems.push("这个浏览器不支持 WebSerial，请改用 Chrome 或 Edge 桌面版（Safari / Firefox 用不了）");
  }
  if (!window.crypto || !crypto.subtle) {
    problems.push("浏览器缺少 WebCrypto，无法校验固件完整性");
  }
  if (problems.length) showBanner("⚠ " + problems.join("；") + "。「使用帮助 → 一、浏览器要求」有详细说明。");
})();

el.btnConnect.addEventListener("click", connect);
el.btnFlash.addEventListener("click", flash);
el.btnDisconnect.addEventListener("click", async () => {
  await releasePort();
  setStatus("已断开。");
});

el.btnHelp.addEventListener("click", openHelp);
el.fileInput.addEventListener("change", () => {
  const f = el.fileInput.files && el.fileInput.files[0];
  if (f) loadLocalFile(f);
});
el.expectHash.addEventListener("change", () => {
  // 校验位填/改之后，拿已选的文件重判一次
  const f = el.fileInput.files && el.fileInput.files[0];
  if (f) loadLocalFile(f);
});
el.btnUseOnline.addEventListener("click", () => {
  if (online) {
    setActive(online);
    el.fileState.textContent = "已改回用线上固件。";
  }
});
el.btnHelp2.addEventListener("click", openHelp);
el.btnHelpClose.addEventListener("click", closeHelp);
el.help.addEventListener("click", (e) => {
  if (e.target === el.help) closeHelp();
});
window.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !el.help.hidden) closeHelp();
});

window.addEventListener("beforeunload", (e) => {
  if (busy) {
    e.preventDefault();
    e.returnValue = "";
  }
});

if (location.hash === "#help") openHelp();

refreshButtons();
initFirmware();
