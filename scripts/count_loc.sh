#!/usr/bin/env bash
# count_loc.sh - 统计 ForgeRelay 自研生产代码行数（需求 SIZE-03）。
#
# 口径（需求 §14）：C/C++ 生产代码；不含测试、示例、文档、构建目录、第三方依赖、
# 自动生成文件、空行和纯注释行。统计范围：include/ 与 src/（含全部子模块）。
# 优先使用 scc，其次 cloc；两者都不可用时退出并提示安装。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGETS=()
for dir in include src; do
    if [ -d "${PROJECT_ROOT}/${dir}" ]; then
        TARGETS+=("${PROJECT_ROOT}/${dir}")
    fi
done
if [ ${#TARGETS[@]} -eq 0 ]; then
    echo "error: no include/ or src/ directory found" >&2
    exit 2
fi

if command -v scc >/dev/null 2>&1; then
    echo "== scc (production code: include/ + src/) =="
    exec scc --no-cocomo --by-file=false "${TARGETS[@]}"
elif command -v cloc >/dev/null 2>&1; then
    echo "== cloc (production code: include/ + src/) =="
    exec cloc --quiet --include-lang=C,C++,C/C++ Header --not-match-f='' "${TARGETS[@]}"
else
    cat >&2 <<'EOF'
error: neither 'scc' nor 'cloc' is installed.
Install one of them to measure production LOC (requirement SIZE-03):
  Ubuntu: sudo apt install cloc   |  Rocky: sudo dnf install cloc
  or:     cargo install sccache?  -> use `scc` from https://github.com/boyter/scc
EOF
    exit 1
fi
