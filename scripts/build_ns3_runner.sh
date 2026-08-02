#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ns3_dir="${project_dir}/.deps/ns-3.47"
runner_source="${project_dir}/src/ns3_runner/afsim-ns3-runner.cc"
scratch_source="${ns3_dir}/scratch/afsim-ns3-runner.cc"
bin_dir="${project_dir}/.deps/bin"
runner_link="${bin_dir}/afsim-ns3-runner"

if [[ ! -x "${ns3_dir}/ns3" ]]; then
    echo "Missing project-local ns-3.47 source: ${ns3_dir}" >&2
    exit 1
fi
if [[ "$(tr -d '[:space:]' < "${ns3_dir}/VERSION")" != "3.47" ]]; then
    echo "The project-local ns-3 source is not version 3.47." >&2
    exit 1
fi
if [[ ! -f "${runner_source}" ]]; then
    echo "Missing runner source: ${runner_source}" >&2
    exit 1
fi

ln -sfn "${runner_source}" "${scratch_source}"
(
    cd "${ns3_dir}"
    ./ns3 configure \
        --build-profile=default \
        --disable-examples \
        --disable-tests \
        --enable-modules="core;network;internet;mobility;point-to-point;applications;flow-monitor"
    ./ns3 build afsim-ns3-runner
)

runner_binary="$(
    find "${ns3_dir}/build" -type f -perm -111 \
        -name '*afsim-ns3-runner*' -print -quit
)"
if [[ -z "${runner_binary}" ]]; then
    echo "Build completed but the runner binary was not found." >&2
    exit 1
fi

mkdir -p "${bin_dir}"
ln -sfn "${runner_binary}" "${runner_link}"
echo "${runner_link}"
