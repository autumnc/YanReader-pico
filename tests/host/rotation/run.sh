#!/bin/sh
# 主机端旋转映射等价性对拍（P4 / P0.2）。不需要 IDF 环境，g++ 即可。
set -e
cd "$(dirname "$0")"
g++ -O2 -o rot_equiv rot_equiv.cpp
./rot_equiv
