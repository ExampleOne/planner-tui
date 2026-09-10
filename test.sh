#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$project_dir/build.sh"
"$project_dir/build/planner-tests"
"$project_dir/build/planner" --demo --plain --render board --size 80x24 \
    --fake-now 2026-08-19T10:00:00 >/dev/null
"$project_dir/build/planner" --demo --plain --render calendar --size 40x12 \
    --fake-now 2026-08-19T10:00:00 >/dev/null
echo "All smoke tests passed."

