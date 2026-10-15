#!/usr/bin/env bash
# StreamAudioFilter — builds and runs the platform independent native tests.
#
# Works on Linux/macOS (g++/clang++) with no Windows SDK at all: the DSP core is
# deliberately free of OS headers. Use this before touching the Windows layer.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/native/build/tests"
mkdir -p "${build_dir}"

cxx="${CXX:-g++}"
flags=(-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread)

echo "compiling native tests with ${cxx}"
"${cxx}" "${flags[@]}" \
    -o "${build_dir}/dsp_tests" \
    "${root}/native/test/dsp_tests.cc" \
    "${root}/native/src/dsp/ring_buffer.cc" \
    "${root}/native/src/dsp/convert.cc" \
    "${root}/native/src/dsp/resampler.cc" \
    "${root}/native/src/dsp/mixer.cc"

exec "${build_dir}/dsp_tests" "$@"
