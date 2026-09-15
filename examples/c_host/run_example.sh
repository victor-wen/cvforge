#!/usr/bin/env bash
# Run the native C host example against a test-enabled build tree.
#
# Usage: examples/c_host/run_example.sh [build-dir] [cycles]
#   build-dir  build tree with the example target (default build/wsl-gcc-debug)
#   cycles     number of inspections to run (default 1)
#
# The script uses the checked-in synthetic configuration under
# examples/c_host/config, creates a throwaway output root, runs the example,
# and forwards its exit code unchanged. The synthetic backend is accepted only
# by builds configured with a Debug-style test-enabled preset such as
# wsl-gcc-debug (CVFORWIN_BUILD_TEST_BACKENDS=ON).
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

build_dir="${1:-${repo_root}/build/wsl-gcc-debug}"
cycles="${2:-1}"

example="${build_dir}/examples/c_host/cvf_c_host_example"
if [[ ! -x "${example}" ]]; then
    echo "run_example: example executable not found at ${example}" >&2
    echo "run_example: build it first, for example 'cmake --build --preset wsl-gcc-debug'" >&2
    exit 2
fi

output_root="$(mktemp -d "${TMPDIR:-/tmp}/cvf-c-host-example.XXXXXX")"
cleanup() { rm -rf "${output_root}"; }
trap cleanup EXIT

echo "run_example: config_root=${script_dir}/config output_root=${output_root}"
"${example}" "${script_dir}/config" "${output_root}" "${cycles}"
