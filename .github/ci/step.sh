#!/usr/bin/env bash
#
# Run one workflow step's script, tee everything to a log file, and — if the
# step fails — publish the tail of that log as an annotation.
#
# Why: GitHub's job log endpoint (blob.core.windows.net) is not reachable from
# every environment, so a step that fails without explaining itself in an
# annotation can leave the cause invisible. Annotations are always reachable.
#
# Usage from a workflow step (the script comes from stdin, so the "run: |"
# block keeps its normal shape):
#
#     - name: Something risky
#       run: |
#         .github/ci/step.sh "Something risky" <<'SCRIPT'
#         set -euo pipefail
#         ...
#         SCRIPT
#
set -uo pipefail

name="${1:-step}"
log="${RUNNER_TEMP:-/tmp}/ci-$(printf '%s' "$name" | tr -c 'A-Za-z0-9' '-').log"
: > "$log"

# The inner shell is where the step's own "set -euo pipefail" applies; this
# wrapper keeps running afterwards so that it can report the failure.
bash -euo pipefail 2>&1 | tee "$log"
status="${PIPESTATUS[0]}"

if [ "$status" -ne 0 ]; then
    tail_log="$(tail -c 1400 "$log" | sed -e 's/%/%25/g' -e 's/\r//g' | tr '\n' '|')"
    echo "::error title=$name failed (exit $status)::log: $log -> $tail_log"
fi

exit "$status"
