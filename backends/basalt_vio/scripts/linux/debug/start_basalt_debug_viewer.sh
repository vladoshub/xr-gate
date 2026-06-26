XR="$HOME/src/xr_tracking/backends/basalt_vio/tools/debug/"

python3 "$XR/xr_basalt_trajectory_viewer.py" \
  --trajectory /tmp/xr_basalt_unified_live/trajectory.csv \
  --flip-x \
  --flip-y
