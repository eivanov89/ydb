#!/usr/bin/env bash
# Start an isolated mock cluster and the Skipper HTTP API.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PYTHONDONTWRITEBYTECODE=1
exec "${YDBD_MOCK_BASE_PYTHON:-python3}" "$HERE/skipper.py" "$@"
