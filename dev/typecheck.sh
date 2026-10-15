#!/usr/bin/env bash
#
# StreamAudioFilter — development helper
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Type-checks and lints the plugin with the real Vencord toolchain, exactly the
# way Vencord's own CI does it (tsc --noEmit + eslint), because a plugin that
# does not compile inside Vencord is worthless.
#
# Usage:
#     VENCORD_DIR=/path/to/Vencord ./dev/typecheck.sh
#
# VENCORD_DIR must point at a Vencord checkout with installed dependencies
# (`pnpm install`). The plugin is copied into src/userplugins/StreamAudioFilter
# of that checkout, which is the standard installation path for user plugins.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENCORD_DIR="${VENCORD_DIR:-/tmp/vencord}"

if [ ! -f "$VENCORD_DIR/tsconfig.json" ]; then
    echo "Vencord checkout not found at '$VENCORD_DIR'." >&2
    echo "Clone Vencord and install its dependencies, then set VENCORD_DIR." >&2
    exit 2
fi

if [ ! -d "$VENCORD_DIR/node_modules" ]; then
    echo "No node_modules in '$VENCORD_DIR'. Run 'pnpm install' there first." >&2
    exit 2
fi

TARGET="$VENCORD_DIR/src/userplugins/StreamAudioFilter"
rm -rf "$TARGET"
mkdir -p "$TARGET"

for item in types.ts native.ts nativeBridge.ts dataStore.ts settings.ts index.tsx apps audio components stream ui tests style.css; do
    [ -e "$ROOT/$item" ] && cp -r "$ROOT/$item" "$TARGET/"
done

cd "$VENCORD_DIR"
echo "== tsc --noEmit =="
npx tsc --noEmit

if [ "${FIX:-0}" = "1" ]; then
    echo "== eslint --fix (results are copied back into the repository) =="
    npx eslint --fix "src/userplugins/StreamAudioFilter" || true
    for item in types.ts native.ts nativeBridge.ts dataStore.ts settings.ts index.tsx apps audio components stream ui tests style.css; do
        [ -e "$TARGET/$item" ] && cp -r "$TARGET/$item" "$ROOT/"
    done
fi

echo "== rule engine tests =="
npx tsx src/userplugins/StreamAudioFilter/tests/rules.test.ts

echo "== eslint =="
npx eslint "src/userplugins/StreamAudioFilter"
echo "== ok =="
