#!/bin/sh
# 主机端对拍：main/gfx/diff_scan.h 的按字差分扫描 == 逐字节扫描。g++ 即可，不需要 IDF 环境。
set -e
cd "$(dirname "$0")"
g++ -O2 -Wall -Wextra -o diff_scan_test diff_scan_test.cpp
./diff_scan_test
