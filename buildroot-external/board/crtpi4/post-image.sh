#!/bin/sh
# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
# Assemble the SD image from our static genimage recipe.
set -e
BOARD_DIR="$(dirname "$0")"
support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
