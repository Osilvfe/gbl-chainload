#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
make -s -C tests/host/helpers test_mode2_runtime
exec tests/host/helpers/test_mode2_runtime
