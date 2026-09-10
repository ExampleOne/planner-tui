#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
local_compiler="$project_dir/.toolchain/root/usr/bin/x86_64-linux-gnu-g++-15"
if [[ -x "$local_compiler" ]]; then
    compiler="$local_compiler"
else
    compiler="$(command -v g++ || true)"
fi
build_dir="$project_dir/build"
if [[ -d "$project_dir/.toolchain/root/usr/lib/x86_64-linux-gnu" ]]; then
    export LD_LIBRARY_PATH="$project_dir/.toolchain/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

if [[ -z "$compiler" || ! -x "$compiler" ]]; then
    echo "No C++ compiler found. Run ./tools/bootstrap-toolchain.sh or install g++." >&2
    exit 1
fi

mkdir -p "$build_dir"

common=(
    -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2
    -I"$project_dir/src"
    -I"$project_dir/.toolchain/root/usr/include"
    -I"$project_dir/.toolchain/root/usr/include/x86_64-linux-gnu"
)
sources=("$project_dir/src/planner.cpp" "$project_dir/src/render.cpp")
sqlite_lib="/usr/lib/x86_64-linux-gnu/libsqlite3.so.0"
ncurses_lib="/usr/lib/x86_64-linux-gnu/libncursesw.so.6"
tinfo_lib="/usr/lib/x86_64-linux-gnu/libtinfo.so.6"

"$compiler" "${common[@]}" "${sources[@]}" "$project_dir/src/main.cpp" \
    "$sqlite_lib" "$ncurses_lib" "$tinfo_lib" -o "$build_dir/planner"
"$compiler" "${common[@]}" "${sources[@]}" "$project_dir/tests/test_planner.cpp" \
    "$sqlite_lib" -o "$build_dir/planner-tests"

echo "Built $build_dir/planner"
