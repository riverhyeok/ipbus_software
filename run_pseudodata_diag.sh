#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
work_dir="${1:-$PWD}"
input_dir="${2:-daq_bin_results}"
output_dir="${3:-pseudo_diag_results}"
source_file="${script_dir}/test_bandwidth_reco_pseudodata_diag.cxx"
binary_file="${script_dir}/test_bandwidth_reco_pseudodata_diag"

if [[ ! -f "${source_file}" ]]; then
    echo "Source file not found: ${source_file}" >&2
    exit 1
fi

echo "[1/2] Building diagnostic analyzer"
g++ -O2 -g -Wall -Wextra -std=c++17 "${source_file}" -o "${binary_file}"

echo "[2/2] Reading ${work_dir}/${input_dir}"
cd -- "${work_dir}"
"${binary_file}" --input "${input_dir}" --output "${output_dir}"

echo "Done: ${work_dir}/${output_dir}"
