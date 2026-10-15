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

dsp_sources=(
    "${root}/native/src/dsp/ring_buffer.cc"
    "${root}/native/src/dsp/convert.cc"
    "${root}/native/src/dsp/resampler.cc"
    "${root}/native/src/dsp/mixer.cc"
)
engine_sources=(
    "${dsp_sources[@]}"
    "${root}/native/src/app_identity.cc"
    "${root}/native/src/capture_group.cc"
    "${root}/native/src/engine.cc"
    "${root}/native/src/mock/mock_backend.cc"
)

echo "compiling native tests with ${cxx}"
"${cxx}" "${flags[@]}" -o "${build_dir}/dsp_tests" "${root}/native/test/dsp_tests.cc" "${dsp_sources[@]}"
"${cxx}" "${flags[@]}" -o "${build_dir}/engine_tests" "${root}/native/test/engine_tests.cc" "${engine_sources[@]}"

status=0
"${build_dir}/dsp_tests" "$@" || status=1
echo
"${build_dir}/engine_tests" "$@" || status=1
exit "${status}"
