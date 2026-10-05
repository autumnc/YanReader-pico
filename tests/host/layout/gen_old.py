#!/usr/bin/env python3
"""从 git HEAD 的 main/ui_helpers.cpp 抽出**改造前**的折行几何，生成 build/old_layout.inc。

为什么是"抽"而不是手抄：这组函数是"内置面下逐像素零变化"（等价性护栏，见
layout_equiv.cpp 的 [4]）的**对照物**。手抄一份放在测试里，改了正文却忘了同步对照物，
护栏就会静默失效 —— 它会一直"绿"，只是绿得没有意义。抽出来就没有这个问题：
对照物永远等于仓库里那个已提交的版本。

抽的范围从 `static int charCellWidth` 到 `buildVrows` 的收尾花括号（HEAD 里这段是连续的）。
函数名加 `old_` 前缀，避免与本文件里从工作区抄来的新版同名；`utf8CharLen` 新旧**逐字
相同**，不重命名、直接用全局那份（抽出来的代码里的调用会解析到它）。

git 不可用（比如把测试目录单独拷走）时**不报错**：删掉 .inc 并让 layout_equiv.cpp 的
`__has_include` 落空，[4]/[6] 两条自动跳过并打印一声。跑得起来比跑得全重要。
"""

import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]           # tests/host/layout -> 仓库根
OUT = HERE / "build" / "old_layout.inc"

# 只有与新代码同名的才需要改；改名后旧代码内部的调用点也一并跟着改。
RENAME = {
    "charCellWidth": "old_charCellWidth",
    "byteToCells": "old_byteToCells",
    "cellsToByte": "old_cellsToByte",
    "mdIndentCells": "old_mdIndentCells",
    "mdPrefixLen": "old_mdPrefixLen",
    "buildVrows": "old_buildVrows",
}


def main() -> int:
    try:
        out = subprocess.run(["git", "-C", str(REPO), "show", "HEAD:main/ui_helpers.cpp"],
                             check=True, capture_output=True, text=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(">> gen_old.py: 取不到 HEAD:main/ui_helpers.cpp（%s）" % (e.__class__.__name__,))
        print(">>             跳过 [4] 内置面等价护栏与 [6] 生效性检查")
        OUT.unlink(missing_ok=True)
        return 0

    start = out.index("static int charCellWidth(unsigned char c) {")
    b = out.index("std::vector<VRow> buildVrows(", start)
    end = out.index("\n}\n", out.index("return vrows;", b)) + len("\n}\n")
    body = out[start:end]

    for new, old in RENAME.items():
        body = re.sub(r"\b%s\b" % new, old, body)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(
        "// 由 gen_old.py 从 git HEAD:main/ui_helpers.cpp 自动抽出（改造前的折行几何）。\n"
        "// 勿手改、勿提交：run.sh 每次跑都会重新生成。\n"
        "namespace old {\n" + body + "\n}  // namespace old\n")
    print(">> gen_old.py: build/old_layout.inc（%d 字节，来自 HEAD 的折行几何）" % len(body))
    return 0


if __name__ == "__main__":
    sys.exit(main())
