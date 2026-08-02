#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"${project_dir}/scripts/build_ns3_runner.sh"

cmake \
    -S "${project_dir}/src/windows_adapter" \
    -B "${project_dir}/.deps/windows-adapter-build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON
cmake --build "${project_dir}/.deps/windows-adapter-build" --parallel
ctest \
    --test-dir "${project_dir}/.deps/windows-adapter-build" \
    --output-on-failure

export PYTHONPATH="${project_dir}/src"
python3 -W error::ResourceWarning -m unittest \
    discover \
    -s "${project_dir}/tests" \
    -p "test_*.py" \
    -v
