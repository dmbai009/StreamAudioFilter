#!/usr/bin/env bash
#
# Vencord, a Discord client mod
# Copyright (c) 2026 Vendicated and contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# StreamAudioFilter — checks the boundary between JavaScript and the native
# engine without Windows: the portable part of the addon (engine, rules, DSP,
# scripted test backend) is compiled into a real .node file for the host system
# and then driven through its actual N-API entry points.
#
# The only external piece is the Node-API header set, which is available as a
# normal npm package:
#
#     npm install --no-save node-api-headers     # or: NODE_API_HEADERS=... ./scripts/run-addon-tests.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/addon-test"
CXX="${CXX:-g++}"

INCLUDE="${NODE_API_HEADERS:-}"
if [ -z "$INCLUDE" ]; then
    for candidate in \
        "$ROOT/node_modules/node-api-headers/include" \
        "$ROOT/../node_modules/node-api-headers/include" \
        "/tmp/tc/node_modules/node-api-headers/include"; do
        if [ -d "$candidate" ]; then
            INCLUDE="$candidate"
            break
        fi
    done
fi

if [ -z "$INCLUDE" ]; then
    echo "Node-API headers not found." >&2
    echo "Install them with:  npm install --no-save node-api-headers" >&2
    echo "or point NODE_API_HEADERS at the include directory." >&2
    exit 2
fi

mkdir -p "$OUT"

echo "== compiling the portable part of the addon with $CXX =="
"$CXX" -std=c++17 -O1 -fPIC -shared -DNAPI_VERSION=8 -I"$INCLUDE" \
    -o "$OUT/streamaudiofilter.node" \
    "$ROOT/native/src/addon.cc" \
    "$ROOT/native/src/engine.cc" \
    "$ROOT/native/src/app_identity.cc" \
    "$ROOT/native/src/platform.cc" \
    "$ROOT/native/src/mock/mock_backend.cc" \
    "$ROOT"/native/src/dsp/*.cc \
    -lpthread

echo "== running the addon tests =="
node "$ROOT/tests/addon.test.mjs" "$OUT/streamaudiofilter.node"
