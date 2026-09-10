#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
debs="$project_dir/.toolchain/debs"
root="$project_dir/.toolchain/root"
mkdir -p "$debs" "$root"

cd "$debs"
apt-get download \
    g++-15-x86-64-linux-gnu gcc-15-x86-64-linux-gnu cpp-15-x86-64-linux-gnu \
    gcc-15-base libstdc++-15-dev libgcc-15-dev libc6-dev linux-libc-dev \
    libc-dev-bin rpcsvc-proto libisl23 libmpc3 libncurses-dev libsqlite3-dev ninja-build

for package_file in ./*.deb; do
    dpkg-deb -x "$package_file" "$root"
done

echo "Project-local GCC installed under $root"

