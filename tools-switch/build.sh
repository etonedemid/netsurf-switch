#!/bin/bash
# Build netsurf.nro inside the tools-switch Docker image.
set -euo pipefail
cd "$(dirname "$0")/.."
./ns-make.sh -j"$(nproc)" "$@"
./tools-switch/package.sh
