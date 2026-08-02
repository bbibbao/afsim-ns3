#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONPATH="${project_dir}/src"
exec python3 "${project_dir}/src/afsim_ns3_service.py" "$@"
