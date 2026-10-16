#!/usr/bin/env bash
# StreamAudioFilter — builds saf-audio-host for non Windows systems (g++/clang++).
# Without Windows there is no WASAPI: the host runs with the signal generator
# and the test output only. Used by CI to test the host's protocol on Linux.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${root}/native/build/host"
mkdir -p "${out}"
cxx="${CXX:-g++}"

src="${root}/native/src"
"${cxx}" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -o "${out}/saf-audio-host" \
    "${src}/host/host_main.cc" "${src}/null_sink.cc" "${src}/app_identity.cc" "${src}/capture_group.cc" \
    "${src}/engine.cc" "${src}/hr_advice.cc" "${src}/platform.cc" "${src}/mock/mock_backend.cc" \
    "${src}/dsp/convert.cc" "${src}/dsp/mixer.cc" "${src}/dsp/resampler.cc" "${src}/dsp/ring_buffer.cc"
echo "built: ${out}/saf-audio-host"
