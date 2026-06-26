#!/bin/bash

BIN="${ROOT_PROJECT:-$HOME/src/xr_tracking/bin}"

cd "$BIN"

./xreal_display_helper/xreal_display_helper --mode 60hz --keep-running
