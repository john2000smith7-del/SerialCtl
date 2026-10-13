#!/usr/bin/env bash
set -euo pipefail
audit_dir="$(cd "$(dirname "$0")/.." && pwd)"
source_dir="$audit_dir/source"
output_dir="$audit_dir/evidence/architecture"
mkdir -p "$output_dir"
{
    date -u +'%Y-%m-%dT%H:%M:%SZ'
    uname -a
    g++ --version
    if git -C "$source_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git -C "$source_dir" rev-parse HEAD
    else
        cat "$audit_dir/evidence/source/review-sha.txt"
    fi
    cat "$source_dir/VERSION"
    g++ -std=c++17 -O2 -Wall -Wextra -I "$source_dir/src/terminal" \
        "$source_dir/src/terminal/TerminalModel.cpp" \
        "$audit_dir/scripts/architecture-terminal-response.cpp" \
        -o "$output_dir/terminal-response-model"
    "$output_dir/terminal-response-model"
    g++ -std=c++17 -O2 -Wall -Wextra -I "$source_dir/src/terminal" \
        "$source_dir/src/terminal/TerminalModel.cpp" \
        "$source_dir/tests/unit/TerminalModelTest.cpp" \
        -o "$output_dir/terminal-model-test"
    "$output_dir/terminal-model-test"
    sha256sum "$output_dir/terminal-response-model" "$output_dir/terminal-model-test"
} 2>&1 | tee "$output_dir/model-run.log"
