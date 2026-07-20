#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}" # project root used to resolve binaries and configs
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")" # project root used to resolve binaries and configs

XR_RUNTIME_ADAPTER_BIN="${XR_RUNTIME_ADAPTER_BIN:-$ROOT_PROJECT/bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter}" # xr_runtime_adapter executable path
XR_RUNTIME_ADAPTER_BIN="$(expand_tilde "$XR_RUNTIME_ADAPTER_BIN")" # xr_runtime_adapter executable path

TRACKING_REGISTRY="${TRACKING_REGISTRY:-/tmp/tracking_streams.json}" # source tracking SHM registry path
RUNTIME_TRACKING_REGISTRY="${RUNTIME_TRACKING_REGISTRY:-/tmp/runtime_tracking_streams.json}" # runtime-output tracking SHM registry path
TRACKING_TRANSFORM_CONFIG="${TRACKING_TRANSFORM_CONFIG:-$ROOT_PROJECT/runtime_adapters/xr_runtime_adapter/configs/xr_21_joint_hand_viewer_verified.json}" # runtime coordinate/hand transform profile
TRACKING_TRANSFORM_CONFIG="$(expand_tilde "$TRACKING_TRANSFORM_CONFIG")" # runtime coordinate/hand transform profile

ADAPTER="${ADAPTER:-logging}" # adapter backend mode used by xr_runtime_adapter
MODE="${MODE:-tick}" # main runtime loop mode
INPUT="${INPUT:-shm}" # primary HMD/hand input transport
TICK_RATE="${TICK_RATE:-90}" # runtime publish/tick rate in Hz
PREDICTION_MS="${PREDICTION_MS:-15}" # pose prediction horizon in milliseconds
PRINT_EVERY="${PRINT_EVERY:-90}" # log every N runtime ticks
DURATION="${DURATION:-0}" # run duration in seconds; 0 means unlimited
#ORIGIN_MODE="${ORIGIN_MODE:-start_pose}"
ORIGIN_MODE="${ORIGIN_MODE:-none}" # runtime origin policy; none keeps upstream origin

HMD_STREAM="${HMD_STREAM:-hmd_pose}" # source HMD pose stream name
HAND_STREAM="${HAND_STREAM:-hand_tracking}" # source hand-tracking stream name
HMD_3DOF_PRIORITY="${HMD_3DOF_PRIORITY:-0}" # prefer 3DoF HMD stream when available
HMD_3DOF_REGISTRY="${HMD_3DOF_REGISTRY:-$TRACKING_REGISTRY}" # 3DoF source registry path
HMD_3DOF_STREAM="${HMD_3DOF_STREAM:-hmd_pose_3dof}" # 3DoF HMD pose stream name
HMD_3DOF_REATTACH_ON_STALE_MS="${HMD_3DOF_REATTACH_ON_STALE_MS:-500}" # reattach 3DoF reader after this stale interval

# Runtime-side HMD pose speed/jump stability filter.
# It runs only on fresh input poses before stale hold-last / SHM reattach logic.
# Distance is centimeters per configured time window. Defaults: 50 cm / 250 ms ~= 2 m/s.
RUNTIME_HMD_POSE_STABILITY_FILTER="${RUNTIME_HMD_POSE_STABILITY_FILTER:-1}" # enable runtime HMD jump/speed filter
RUNTIME_HMD_POSE_STABILITY_WINDOW_MS="${RUNTIME_HMD_POSE_STABILITY_WINDOW_MS:-250}" # HMD stability filter time window
RUNTIME_HMD_POSE_STABILITY_MAX_DISTANCE_CM="${RUNTIME_HMD_POSE_STABILITY_MAX_DISTANCE_CM:-50}" # max HMD movement over stability window

BODY_TRACKERS_INPUT="${BODY_TRACKERS_INPUT:-none}"   # body tracker input transport;  none, shm, udp
BODY_TRACKERS_REGISTRY="${BODY_TRACKERS_REGISTRY:-$TRACKING_REGISTRY}" # body tracker source registry path
BODY_TRACKERS_STREAM="${BODY_TRACKERS_STREAM:-body_trackers}" # body tracker source stream name
BODY_TRACKERS_UDP_BIND_HOST="${BODY_TRACKERS_UDP_BIND_HOST:-0.0.0.0}" # UDP bind host for body tracker input
BODY_TRACKERS_UDP_BIND_PORT="${BODY_TRACKERS_UDP_BIND_PORT:-45676}" # UDP bind port for body tracker input
BODY_TRACKERS_REATTACH_ON_STALE_MS="${BODY_TRACKERS_REATTACH_ON_STALE_MS:-1000}" # reattach body tracker reader after stale interval
PUBLISH_RUNTIME_BODY_TRACKERS="${PUBLISH_RUNTIME_BODY_TRACKERS:-0}" # publish runtime body tracker SHM output
RUNTIME_BODY_TRACKER_STABILITY_GATE="${RUNTIME_BODY_TRACKER_STABILITY_GATE:-1}" # enable runtime-side hold/prediction for body trackers; disabled by default

RUNTIME_BODY_TRACKER_MAX_JUMP_M="${RUNTIME_BODY_TRACKER_MAX_JUMP_M:-0.85}" # max observed body tracker jump from last good pose in metres; <=0 disables
RUNTIME_BODY_TRACKER_HOLD_LOST_MS="${RUNTIME_BODY_TRACKER_HOLD_LOST_MS:-150}" # hold last valid body tracker pose after tracker loss
RUNTIME_BODY_TRACKER_PREDICT_LOST_MS="${RUNTIME_BODY_TRACKER_PREDICT_LOST_MS:-350}" # predict body tracker pose after hold-lost phase
RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS="${RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS:-0.8}" # cap body tracker prediction velocity
RUNTIME_BODY_TRACKER_MAX_PREDICTION_ACCELERATION_MPS2="${RUNTIME_BODY_TRACKER_MAX_PREDICTION_ACCELERATION_MPS2:-0}" # cap body tracker prediction acceleration; 0 disables
RUNTIME_BODY_TRACKER_PREDICTION_DAMPING="${RUNTIME_BODY_TRACKER_PREDICTION_DAMPING:-0.8}" # velocity damping during body tracker prediction
RUNTIME_BODY_TRACKER_MAX_PREDICTION_PATH_M="${RUNTIME_BODY_TRACKER_MAX_PREDICTION_PATH_M:-0.5}" # maximum accumulated body-tracker prediction path; 0 disables
RUNTIME_BODY_TRACKER_MAX_DISTANCE_M="${RUNTIME_BODY_TRACKER_MAX_DISTANCE_M:-1.2}" # stop body-tracker prediction when farther than this from adjusted runtime HMD reference; 0 disables
RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_AXIS="${RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_AXIS:-y}" # none, x, y, z; axis offset is applied after HMD transform/config offset
RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_OFFSET_M="${RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_OFFSET_M:--0.8}" # signed offset along selected runtime axis; y=-0.8 moves reference from head to torso center
RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MODE="${RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MODE:-1}" # 1 estimates prediction velocity from all real poses in a rolling window
RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MS="${RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MS:-100}" # body tracker rolling pose-history duration
RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY="${RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY:-1}" # publish decaying linear velocity during body tracker prediction; 0 avoids downstream double prediction
RUNTIME_BODY_TRACKER_REACQUIRE_BLEND_MS="${RUNTIME_BODY_TRACKER_REACQUIRE_BLEND_MS:-180}" # blend from last predicted pose when tracker returns during prediction; 0 disables
RUNTIME_BODY_TRACKER_PREDICTION_PUBLISH_HZ="${RUNTIME_BODY_TRACKER_PREDICTION_PUBLISH_HZ:-90}" # synthetic publish rate while source body tracker stream is stale
RUNTIME_BODY_TRACKER_PREDICTED_STATUS="${RUNTIME_BODY_TRACKER_PREDICTED_STATUS:-tracking}" # predicted body tracker status; tracking, stale, lost

SPATIAL_PROXY_MESH_INPUT="${SPATIAL_PROXY_MESH_INPUT:-shm}"   # spatial proxy mesh input transport;  none, shm, udp
# Source stream consumed by xr_runtime_adapter. Keep the legacy
# SPATIAL_PROXY_MESH_REGISTRY/STREAM names as aliases, but expose SOURCE_* in
# logs/scripts so it is not confused with runtime_spatial_proxy_mesh output.
SPATIAL_PROXY_MESH_SOURCE_REGISTRY="${SPATIAL_PROXY_MESH_SOURCE_REGISTRY:-${SPATIAL_PROXY_MESH_REGISTRY:-$RUNTIME_TRACKING_REGISTRY}}" # source registry for spatial proxy mesh input
SPATIAL_PROXY_MESH_SOURCE_STREAM="${SPATIAL_PROXY_MESH_SOURCE_STREAM:-${SPATIAL_PROXY_MESH_STREAM:-spatial_proxy_mesh}}" # source stream for spatial proxy mesh input
SPATIAL_PROXY_MESH_REGISTRY="$SPATIAL_PROXY_MESH_SOURCE_REGISTRY" # legacy alias for spatial proxy mesh source registry
SPATIAL_PROXY_MESH_STREAM="$SPATIAL_PROXY_MESH_SOURCE_STREAM" # legacy alias for spatial proxy mesh source stream
SPATIAL_PROXY_MESH_UDP_BIND_HOST="${SPATIAL_PROXY_MESH_UDP_BIND_HOST:-0.0.0.0}" # UDP bind host for spatial proxy mesh input
SPATIAL_PROXY_MESH_UDP_BIND_PORT="${SPATIAL_PROXY_MESH_UDP_BIND_PORT:-45740}" # UDP bind port for spatial proxy mesh input
SPATIAL_PROXY_MESH_REATTACH_ON_STALE_MS="${SPATIAL_PROXY_MESH_REATTACH_ON_STALE_MS:-1000}" # reattach spatial mesh reader after stale interval
SPATIAL_PROXY_MESH_MAX_SOURCE_AGE_MS="${SPATIAL_PROXY_MESH_MAX_SOURCE_AGE_MS:-1000}" # max accepted age of source spatial mesh frames
SPATIAL_PROXY_MESH_TRIANGLE_WINDING="${SPATIAL_PROXY_MESH_TRIANGLE_WINDING:-auto}" # triangle winding policy for mesh output;  auto, keep, swap
SPATIAL_PROXY_MESH_ROTATE_DEG_X="${SPATIAL_PROXY_MESH_ROTATE_DEG_X:-0}" # extra spatial mesh rotation around X axis
SPATIAL_PROXY_MESH_ROTATE_DEG_Y="${SPATIAL_PROXY_MESH_ROTATE_DEG_Y:-0}" # extra spatial mesh rotation around Y axis
SPATIAL_PROXY_MESH_ROTATE_DEG_Z="${SPATIAL_PROXY_MESH_ROTATE_DEG_Z:-0}" # extra spatial mesh rotation around Z axis
PUBLISH_RUNTIME_SPATIAL_PROXY_MESH="${PUBLISH_RUNTIME_SPATIAL_PROXY_MESH:-1}" # publish runtime spatial proxy mesh SHM output
RUNTIME_SPATIAL_PROXY_MESH_REGISTRY="${RUNTIME_SPATIAL_PROXY_MESH_REGISTRY:-$RUNTIME_TRACKING_REGISTRY}" # runtime spatial mesh registry path
RUNTIME_SPATIAL_PROXY_MESH_STREAM="${RUNTIME_SPATIAL_PROXY_MESH_STREAM:-runtime_spatial_proxy_mesh}" # runtime spatial mesh stream name
RUNTIME_SPATIAL_PROXY_MESH_SHM_NAME="${RUNTIME_SPATIAL_PROXY_MESH_SHM_NAME:-runtime_spatial_proxy_mesh}" # runtime spatial mesh SHM object name
RUNTIME_BODY_TRACKERS_REGISTRY="${RUNTIME_BODY_TRACKERS_REGISTRY:-$RUNTIME_TRACKING_REGISTRY}" # runtime body tracker registry path
RUNTIME_BODY_TRACKERS_STREAM="${RUNTIME_BODY_TRACKERS_STREAM:-runtime_body_trackers}" # runtime body tracker stream name
RUNTIME_BODY_TRACKERS_SHM_NAME="${RUNTIME_BODY_TRACKERS_SHM_NAME:-runtime_body_trackers}" # runtime body tracker SHM object name

HAND_SKELETON26_INPUT="${HAND_SKELETON26_INPUT:-none}"   # 26-joint hand skeleton input transport;  none, shm, tcp
HAND_SKELETON26_REGISTRY="${HAND_SKELETON26_REGISTRY:-$TRACKING_REGISTRY}" # 26-joint skeleton source registry path
HAND_SKELETON26_STREAM="${HAND_SKELETON26_STREAM:-hand_skeleton26}" # 26-joint skeleton source stream name
HAND_SKELETON26_TCP_HOST="${HAND_SKELETON26_TCP_HOST:-127.0.0.1}" # TCP host for skeleton26 input
HAND_SKELETON26_TCP_PORT="${HAND_SKELETON26_TCP_PORT:-45674}" # TCP port for skeleton26 input
RUNTIME_DERIVE_HAND_GESTURES="${RUNTIME_DERIVE_HAND_GESTURES:-1}" # derive pinch/grab gestures in runtime adapter
HAND_SKELETON26_DERIVE_GESTURES="${HAND_SKELETON26_DERIVE_GESTURES:-$RUNTIME_DERIVE_HAND_GESTURES}" # derive gestures from skeleton26 input
RUNTIME_DERIVE_HAND_GESTURES_WITH_CONTROLLER_INPUT="${RUNTIME_DERIVE_HAND_GESTURES_WITH_CONTROLLER_INPUT:-0}" # allow hand-derived gestures while controller input is active
RUNTIME_DERIVED_GESTURES_REQUIRE_FRESH_TRACKING="${RUNTIME_DERIVED_GESTURES_REQUIRE_FRESH_TRACKING:-1}" # require fresh tracking before deriving gestures
RUNTIME_DERIVED_GESTURE_LATCH_MS="${RUNTIME_DERIVED_GESTURE_LATCH_MS:-60}" # latch derived gestures across brief tracking gaps
RUNTIME_IGNORE_BACKEND_HAND_GESTURES="${RUNTIME_IGNORE_BACKEND_HAND_GESTURES:-1}" # ignore backend-provided gesture fields and derive in runtime
RUNTIME_LEFT_HAND_GESTURES_ENABLED="${RUNTIME_LEFT_HAND_GESTURES_ENABLED:-1}" # enable runtime gestures for left hand
RUNTIME_RIGHT_HAND_GESTURES_ENABLED="${RUNTIME_RIGHT_HAND_GESTURES_ENABLED:-1}" # enable runtime gestures for right hand
RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS="${RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS:-1}" # derive extra buttons from hand gestures
RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS_WITH_CONTROLLER_INPUT="${RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS_WITH_CONTROLLER_INPUT:-0}" # allow extra derived buttons while controller input is active
DERIVED_THUMBS_UP_BUTTON="${DERIVED_THUMBS_UP_BUTTON:-a}" # controller button mapped from thumbs-up gesture
DERIVED_INDEX_POINT_BUTTON="${DERIVED_INDEX_POINT_BUTTON:-${DERIVED_VICTORY_BUTTON:-b}}" # controller button mapped from index-point gesture
DERIVED_THUMBS_UP_ACTIVE_THRESHOLD="${DERIVED_THUMBS_UP_ACTIVE_THRESHOLD:-0.80}" # activation threshold for thumbs-up gesture
DERIVED_INDEX_POINT_ACTIVE_THRESHOLD="${DERIVED_INDEX_POINT_ACTIVE_THRESHOLD:-${DERIVED_VICTORY_ACTIVE_THRESHOLD:-0.80}}" # activation threshold for index-point gesture
DERIVED_THUMBS_UP_DEACTIVE_THRESHOLD="${DERIVED_THUMBS_UP_DEACTIVE_THRESHOLD:-0.45}" # deactivation threshold for thumbs-up gesture
DERIVED_INDEX_POINT_DEACTIVE_THRESHOLD="${DERIVED_INDEX_POINT_DEACTIVE_THRESHOLD:-${DERIVED_VICTORY_DEACTIVE_THRESHOLD:-0.45}}" # deactivation threshold for index-point gesture
DERIVED_EXTRA_GESTURE_RESPONSE_START="${DERIVED_EXTRA_GESTURE_RESPONSE_START:-0.65}" # response curve start for extra gesture buttons
DERIVED_EXTRA_GESTURE_HOLD_MS="${DERIVED_EXTRA_GESTURE_HOLD_MS:-120}" # minimum hold time for extra gesture buttons
DERIVED_PINCH_ACTIVE_THRESHOLD="${DERIVED_PINCH_ACTIVE_THRESHOLD:-0.90}" # activation threshold for derived pinch
DERIVED_GRAB_ACTIVE_THRESHOLD="${DERIVED_GRAB_ACTIVE_THRESHOLD:-0.98}" # activation threshold for derived grab
DERIVED_PINCH_DEACTIVE_THRESHOLD="${DERIVED_PINCH_DEACTIVE_THRESHOLD:-0.05}" # deactivation threshold for derived pinch
DERIVED_GRAB_DEACTIVE_THRESHOLD="${DERIVED_GRAB_DEACTIVE_THRESHOLD:-0.20}" # deactivation threshold for derived grab
DERIVED_PINCH_RESPONSE_START="${DERIVED_PINCH_RESPONSE_START:-0.90}" # response curve start for pinch
DERIVED_GRAB_RESPONSE_START="${DERIVED_GRAB_RESPONSE_START:-0.70}" # response curve start for grab

# Runtime-side hand pose stability gate. This replaces the old Mercury-backend gate.
# Keep Mercury backend raw; apply hold/reacquire/jump policy here, before runtime transforms
# and before controller override. Physical controller buttons still apply after this gate.
RUNTIME_HAND_STABILITY_GATE="${RUNTIME_HAND_STABILITY_GATE:-1}" # enable runtime hand pose stability gate
RUNTIME_HAND_GATE_MAX_JUMP_M="${RUNTIME_HAND_GATE_MAX_JUMP_M:-0.30}" # max allowed hand jump before reacquire gating
RUNTIME_HAND_GATE_CONFIRM_FRAMES="${RUNTIME_HAND_GATE_CONFIRM_FRAMES:-5}" # frames required to confirm reacquired hand
RUNTIME_HAND_GATE_CONFIRM_MAX_STEP_M="${RUNTIME_HAND_GATE_CONFIRM_MAX_STEP_M:-0.30}" # max per-frame step while confirming reacquire
RUNTIME_HAND_GATE_HOLD_LOST_MS_HAND_TRACKING="${RUNTIME_HAND_GATE_HOLD_LOST_MS_HAND_TRACKING:-${RUNTIME_HAND_GATE_HOLD_LOST_MS:-60}}" # HAND_TRACKING profile hold last valid hand pose after tracking loss
RUNTIME_HAND_GATE_HOLD_LOST_MS_IMU="${RUNTIME_HAND_GATE_HOLD_LOST_MS_IMU:-${RUNTIME_HAND_GATE_HOLD_LOST_MS:-30}}" # IMU profile hold last valid hand pose after tracking loss
RUNTIME_HAND_GATE_PREDICT_LOST_MS_HAND_TRACKING="${RUNTIME_HAND_GATE_PREDICT_LOST_MS_HAND_TRACKING:-${RUNTIME_HAND_GATE_PREDICT_LOST_MS:-550}}" # HAND_TRACKING profile predict hand pose for this time after hold-lost phase
RUNTIME_HAND_GATE_PREDICT_LOST_MS_IMU="${RUNTIME_HAND_GATE_PREDICT_LOST_MS_IMU:-${RUNTIME_HAND_GATE_PREDICT_LOST_MS:-600}}" # IMU profile predict hand pose for this time after hold-lost phase
RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS="${RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS:-3.0}" # cap prediction velocity for lost hands
RUNTIME_HAND_GATE_PREDICTION_DAMPING="${RUNTIME_HAND_GATE_PREDICTION_DAMPING:-1}" # velocity damping during lost-hand prediction
RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MODE="${RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MODE:-1}" # 1 estimates HAND_TRACKING prediction velocity from a rolling real-pose window
RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MS="${RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MS:-100}" # HAND_TRACKING rolling pose-history duration
RUNTIME_HAND_TRACKING_MAX_PREDICTION_PATH_M="${RUNTIME_HAND_TRACKING_MAX_PREDICTION_PATH_M:-0.5}" # maximum accumulated HAND_TRACKING prediction path; 0 disables
RUNTIME_HAND_TRACKING_MAX_DISTANCE_M="${RUNTIME_HAND_TRACKING_MAX_DISTANCE_M:-0.6}" # stop HAND_TRACKING prediction when farther than this from adjusted runtime HMD reference; 0 disables
RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_AXIS="${RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_AXIS:-none}" # none, x, y, z; none keeps the transformed HMD position unchanged
RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_OFFSET_M="${RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_OFFSET_M:-0}" # signed runtime-axis offset applied after HMD transform/config offset
RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY="${RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY:-1}" # publish decaying linear velocity during hand prediction; 0 avoids downstream double prediction
RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_HAND_TRACKING="${RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_HAND_TRACKING:-${RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS:-180}}" # HAND_TRACKING profile blend duration after reacquire
RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_IMU="${RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_IMU:-${RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS:-180}}" # IMU profile blend duration after reacquire
RUNTIME_HAND_GATE_DEBUG_CSV="${RUNTIME_HAND_GATE_DEBUG_CSV:-}" # optional CSV path for hand gate diagnostics
RUNTIME_HAND_GATE_MAX_CONTINUITY_VELOCITY_MPS="${RUNTIME_HAND_GATE_MAX_CONTINUITY_VELOCITY_MPS:-7.0}" # max continuity velocity for hand gate

# Runtime-side position/orientation deadband jitter filter.
# It suppresses very small pose changes while preserving larger real movement.
# Position values are centimeters; orientation values are degrees.
RUNTIME_JITTER_FILTER="${RUNTIME_JITTER_FILTER:-1}" # enable runtime pose jitter deadband filter
RUNTIME_JITTER_FILTER_HMD_CM="${RUNTIME_JITTER_FILTER_HMD_CM:-0.15}" # HMD position jitter deadband in cm
RUNTIME_JITTER_FILTER_TRACKER_CM="${RUNTIME_JITTER_FILTER_TRACKER_CM:-0.25}" # tracker/hand position jitter deadband in cm; position always comes from hand tracking
RUNTIME_JITTER_FILTER_HMD_DEG="${RUNTIME_JITTER_FILTER_HMD_DEG:-0.10}" # HMD orientation jitter deadband in degrees
RUNTIME_JITTER_FILTER_TRACKER_DEG_HAND_TRACKING="${RUNTIME_JITTER_FILTER_TRACKER_DEG_HAND_TRACKING:-${RUNTIME_JITTER_FILTER_TRACKER_DEG:-1.0}}" # HAND_TRACKING profile tracker/hand orientation jitter deadband in degrees
RUNTIME_JITTER_FILTER_TRACKER_DEG_IMU="${RUNTIME_JITTER_FILTER_TRACKER_DEG_IMU:-${RUNTIME_JITTER_FILTER_TRACKER_DEG:-0.0}}" # IMU profile tracker/hand orientation jitter deadband in degrees


RUNTIME_JITTER_FILTER_HMD_VEL_CM_S="${RUNTIME_JITTER_FILTER_HMD_VEL_CM_S:-1.0}" # HMD linear velocity zero-deadband in cm/s
RUNTIME_JITTER_FILTER_TRACKER_VEL_CM_S="${RUNTIME_JITTER_FILTER_TRACKER_VEL_CM_S:-2.0}" # tracker/hand linear velocity zero-deadband in cm/s; linear velocity comes from hand tracking
RUNTIME_JITTER_FILTER_HMD_ANG_VEL_DEG_S="${RUNTIME_JITTER_FILTER_HMD_ANG_VEL_DEG_S:-1.0}" # HMD angular velocity zero-deadband in deg/s
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_HAND_TRACKING="${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_HAND_TRACKING:-${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S:-2.5}}" # HAND_TRACKING profile tracker/hand angular velocity zero-deadband in deg/s
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_IMU="${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_IMU:-${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S:-0.0}}" # IMU profile tracker/hand angular velocity zero-deadband in deg/s


RUNTIME_JITTER_FILTER_HMD_VEL_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_HMD_VEL_SMOOTH_ALPHA:-1.0}" # HMD linear velocity smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_TRACKER_VEL_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_TRACKER_VEL_SMOOTH_ALPHA:-0.70}" # tracker/hand linear velocity smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_HMD_ANG_VEL_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_HMD_ANG_VEL_SMOOTH_ALPHA:-1.0}" # HMD angular velocity smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_HAND_TRACKING="${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_HAND_TRACKING:-${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA:-0.10}}" # HAND_TRACKING profile tracker/hand angular velocity smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_IMU="${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_IMU:-${RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA:-1.0}}" # IMU profile tracker/hand angular velocity smoothing alpha; 1 disables, lower is smoother

RUNTIME_JITTER_FILTER_HMD_POS_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_HMD_POS_SMOOTH_ALPHA:-1.0}" # HMD position smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_TRACKER_POS_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_TRACKER_POS_SMOOTH_ALPHA:-0.75}" # tracker/hand position smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_HMD_ROT_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_HMD_ROT_SMOOTH_ALPHA:-1.0}" # HMD orientation smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_HAND_TRACKING="${RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_HAND_TRACKING:-${RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA:-0.70}}" # HAND_TRACKING profile tracker/hand orientation smoothing alpha; 1 disables, lower is smoother
RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_IMU="${RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_IMU:-${RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA:-1.0}}" # IMU profile tracker/hand orientation smoothing alpha; 1 disables, lower is smoother

# Body trackers use a fully independent jitter profile. Defaults inherit the
# existing tracker/HAND_TRACKING values so applying this patch does not change behavior.
RUNTIME_JITTER_FILTER_BODY_TRACKER="${RUNTIME_JITTER_FILTER_BODY_TRACKER:-$RUNTIME_JITTER_FILTER}" # enable body tracker jitter filtering independently
RUNTIME_JITTER_FILTER_BODY_TRACKER_CM="${RUNTIME_JITTER_FILTER_BODY_TRACKER_CM:-2.0}" # body tracker position jitter deadband in cm
RUNTIME_JITTER_FILTER_BODY_TRACKER_DEG="${RUNTIME_JITTER_FILTER_BODY_TRACKER_DEG:-2.0}" # body tracker orientation jitter deadband in degrees
RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_CM_S="${RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_CM_S:-2.0}" # body tracker linear velocity zero-deadband in cm/s
RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_DEG_S="${RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_DEG_S:-1.0}" # body tracker angular velocity zero-deadband in deg/s
RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_SMOOTH_ALPHA:-1.0}" # body tracker linear velocity smoothing alpha
RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_SMOOTH_ALPHA:-1.0}" # body tracker angular velocity smoothing alpha
RUNTIME_JITTER_FILTER_BODY_TRACKER_POS_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_BODY_TRACKER_POS_SMOOTH_ALPHA:-1.0}" # body tracker position smoothing alpha
RUNTIME_JITTER_FILTER_BODY_TRACKER_ROT_SMOOTH_ALPHA="${RUNTIME_JITTER_FILTER_BODY_TRACKER_ROT_SMOOTH_ALPHA:-1.0}" # body tracker orientation smoothing alpha
# Per-controller orientation source. IMU mode automatically falls back per side
# to HAND_TRACKING_BACKEND while current valid IMU orientation is unavailable.
RUNTIME_CONTROLLER_LEFT_ORIENTATION_SOURCE="${RUNTIME_CONTROLLER_LEFT_ORIENTATION_SOURCE:-IMU_OVERRIDE_CONTROLLER_RUNTIME}"
RUNTIME_CONTROLLER_RIGHT_ORIENTATION_SOURCE="${RUNTIME_CONTROLLER_RIGHT_ORIENTATION_SOURCE:-IMU_OVERRIDE_CONTROLLER_RUNTIME}"
HAND_ORIENTATION_OFFSET_ONLY_RUNTIME="${HAND_ORIENTATION_OFFSET_ONLY_RUNTIME:-1}" # apply hand_orientation_offset only for sides present in fresh override_controller input

# Optional IMU motion features. Global values can enable both sides; per-side
# values override them. Every feature also requires the corresponding side to
# actually use IMU_OVERRIDE_CONTROLLER_RUNTIME with current valid sensor data.
RUNTIME_CONTROLLER_IMU_ACCELERATION_INTEGRATION="${RUNTIME_CONTROLLER_IMU_ACCELERATION_INTEGRATION:-1}"
RUNTIME_CONTROLLER_LEFT_IMU_ACCELERATION_INTEGRATION="${RUNTIME_CONTROLLER_LEFT_IMU_ACCELERATION_INTEGRATION:-$RUNTIME_CONTROLLER_IMU_ACCELERATION_INTEGRATION}"
RUNTIME_CONTROLLER_RIGHT_IMU_ACCELERATION_INTEGRATION="${RUNTIME_CONTROLLER_RIGHT_IMU_ACCELERATION_INTEGRATION:-$RUNTIME_CONTROLLER_IMU_ACCELERATION_INTEGRATION}"
RUNTIME_CONTROLLER_IMU_POSITION_PREDICTION="${RUNTIME_CONTROLLER_IMU_POSITION_PREDICTION:-1}"
RUNTIME_CONTROLLER_LEFT_IMU_POSITION_PREDICTION="${RUNTIME_CONTROLLER_LEFT_IMU_POSITION_PREDICTION:-$RUNTIME_CONTROLLER_IMU_POSITION_PREDICTION}"
RUNTIME_CONTROLLER_RIGHT_IMU_POSITION_PREDICTION="${RUNTIME_CONTROLLER_RIGHT_IMU_POSITION_PREDICTION:-$RUNTIME_CONTROLLER_IMU_POSITION_PREDICTION}"
RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MODE="${RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MODE:-1}" # 1 estimates IMU-controller base velocity from a rolling optical-pose window
RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MS="${RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MS:-100}" # IMU-controller rolling optical-pose history duration
RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS="${RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS:-2.0}" # cap final IMU-controller predicted position speed after acceleration/lever arm; 0 disables
RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_PATH_M="${RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_PATH_M:-0.5}" # maximum accumulated IMU-controller prediction path; 0 disables
RUNTIME_CONTROLLER_IMU_MAX_DISTANCE_M="${RUNTIME_CONTROLLER_IMU_MAX_DISTANCE_M:-0.6}" # stop IMU-controller prediction when farther than this from adjusted runtime HMD reference; 0 disables
RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_AXIS="${RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_AXIS:-none}" # none, x, y, z; none keeps the transformed HMD position unchanged
RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_OFFSET_M="${RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_OFFSET_M:-0}" # signed runtime-axis offset applied after HMD transform/config offset
RUNTIME_CONTROLLER_IMU_LEVER_ARM_MODE="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_MODE:-1}" # 1 curves only the Predicting-state trajectory from live IMU orientation
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_X_M="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_X_M:-0}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Y_M="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Y_M:-0}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Z_M="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Z_M:--0.12}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_X_M="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_X_M:-0}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Y_M="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Y_M:-0}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Z_M="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Z_M:--0.12}"
# Optional removal of rotational acceleration already represented by the
# lever-arm trajectory. Exact IMU board placement is unknown, so the same
# per-side lever-arm vector is used as a pivot-to-sensor approximation.
RUNTIME_CONTROLLER_IMU_LEVER_ARM_CENTRIPETAL_COMPENSATION="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_CENTRIPETAL_COMPENSATION:-0}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_TANGENTIAL_COMPENSATION="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_TANGENTIAL_COMPENSATION:-0}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_ANGULAR_ACCELERATION_SMOOTH_ALPHA="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_ANGULAR_ACCELERATION_SMOOTH_ALPHA:-0.15}"
RUNTIME_CONTROLLER_IMU_LEVER_ARM_MAX_ANGULAR_ACCELERATION_RAD_S2="${RUNTIME_CONTROLLER_IMU_LEVER_ARM_MAX_ANGULAR_ACCELERATION_RAD_S2:-50.0}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION:-1}"
RUNTIME_CONTROLLER_LEFT_IMU_YAW_CORRECTION="${RUNTIME_CONTROLLER_LEFT_IMU_YAW_CORRECTION:-$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION}"
RUNTIME_CONTROLLER_RIGHT_IMU_YAW_CORRECTION="${RUNTIME_CONTROLLER_RIGHT_IMU_YAW_CORRECTION:-$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION}"
XR_BACKEND_CONTROL_FILE="${XR_BACKEND_CONTROL_FILE:-/tmp/xr_backend_control.json}" # shared backend control JSON; controller IMU reads gravity_magnitude and reset_counter
RUNTIME_CONTROLLER_IMU_CONTROL_POLL_MS="${RUNTIME_CONTROLLER_IMU_CONTROL_POLL_MS:-500}" # poll backend control JSON for reset_counter changes; 0 disables runtime polling
RUNTIME_CONTROLLER_IMU_ACCELERATION_DEADBAND_MPS2="${RUNTIME_CONTROLLER_IMU_ACCELERATION_DEADBAND_MPS2:-0.15}"
RUNTIME_CONTROLLER_IMU_MAX_LINEAR_ACCELERATION_MPS2="${RUNTIME_CONTROLLER_IMU_MAX_LINEAR_ACCELERATION_MPS2:-1.0}"
# IMU position prediction uses the same hold/predict/reacquire timing, velocity
# cap, damping and publish-velocity policy as the IMU hand-gate profile above.
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS:-1}" # periodic correction enabled
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE:-1}"
# Periodic correction uses the latest optical/IMU difference. With the trigger
# filter enabled, that latest residual must stay above DEADBAND_DEG for
# TRIGGER_HOLD_MS while its range stays within TRIGGER_MAX_RANGE_DEG. Exceeding
# the range restarts the complete hold window. Disabling the filter applies the
# latest target immediately when it exceeds DEADBAND_DEG after INTERVAL_MS.
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_DEADBAND_DEG="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_DEADBAND_DEG:-22.5}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_BLEND_MS="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_BLEND_MS:-250}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_FILTER="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_FILTER:-1}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_HOLD_MS="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_HOLD_MS:-650}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_MAX_RANGE_DEG="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_MAX_RANGE_DEG:-10.0}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_INTERVAL_MS="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_INTERVAL_MS:-100}"
# Reacquire correction has an independent threshold and blend duration.
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_DEADBAND_DEG="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_DEADBAND_DEG:-1.0}"
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_BLEND_MS="${RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_BLEND_MS:-250}"

# Controller locomotion space, duplicated for both tracking profiles.
RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_HAND_TRACKING="${RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_HAND_TRACKING:-${RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE:-hmd_pose}}"
RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_HAND_TRACKING="${RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_HAND_TRACKING:-${RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE:-controller}}"
RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_IMU="${RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_IMU:-${RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE:-controller}}"
RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_IMU="${RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_IMU:-${RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE:-controller}}"
CONTROLLER_INPUT_MODE="${CONTROLLER_INPUT_MODE:-controller_buttons_runtime_only}" # controller/hand gesture merge mode; hand_tracking_only #controller_buttons_only #hand_plus_controller №controller_buttons_runtime_only
CONTROLLER_INPUT_TRANSPORT="${CONTROLLER_INPUT_TRANSPORT:-shm}"  # controller input transport;  none, shm, tcp
CONTROLLER_INPUT_REGISTRY="${CONTROLLER_INPUT_REGISTRY:-$TRACKING_REGISTRY}" # controller input source registry path
CONTROLLER_INPUT_STREAM="${CONTROLLER_INPUT_STREAM:-controller_input}" # controller input source stream name
CONTROLLER_INPUT_HOST="${CONTROLLER_INPUT_HOST:-127.0.0.1}" # TCP host for controller input
CONTROLLER_INPUT_PORT="${CONTROLLER_INPUT_PORT:-45672}" # TCP port for controller input
CONTROLLER_INPUT_CONFLICT_POLICY="${CONTROLLER_INPUT_CONFLICT_POLICY:-controller_override}" # policy when controller input conflicts with hand gestures
CONTROLLER_INPUT_STALE_POLICY="${CONTROLLER_INPUT_STALE_POLICY:-hold_last}" # keep last controller state through brief no-hands/controller-source stalls
OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT="${OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT:-1}" # in controller_buttons_runtime_only, block hand gesture input while override_controller stream is fresh/recently seen
OVERRIDE_CONTROLLER_GESTURE_BLOCK_LATCH_MS="${OVERRIDE_CONTROLLER_GESTURE_BLOCK_LATCH_MS:-2000}" # keep gestures blocked this long after last fresh override_controller frame
# How long xr_runtime_adapter keeps the latest controller_input frame fresh.
# override_controller reattach/rescan can briefly pause publishing; keeping this
# above that pause avoids tiny synchronized controller release glitches.
MAX_CONTROLLER_AGE_MS="${MAX_CONTROLLER_AGE_MS:-${CONTROLLER_INPUT_MAX_AGE_MS:-3000}}" # max age before controller_input is considered stale
# pose_invalid: old behavior; hmd_relative_with_input: when hand tracking is lost but override_controller input is active,
# synthesize a body/HMD-relative controller pose instead of leaving hands at an invalid/floor pose.
RUNTIME_CONTROLLER_LOST_HAND_POSE_FALLBACK="${RUNTIME_CONTROLLER_LOST_HAND_POSE_FALLBACK:-hmd_relative_with_input}" # fallback pose policy when hand tracking is lost

CONTROLLER_TRIGGER_PINCH_THRESHOLD="${CONTROLLER_TRIGGER_PINCH_THRESHOLD:-0.55}" # trigger threshold used as pinch/button input
CONTROLLER_GRIP_GRAB_THRESHOLD="${CONTROLLER_GRIP_GRAB_THRESHOLD:-0.55}" # grip threshold used as grab/button input

RUNTIME_POSE_STREAM="${RUNTIME_POSE_STREAM:-runtime_hmd_pose}" # runtime HMD pose output stream name
RUNTIME_POSE_SHM_NAME="${RUNTIME_POSE_SHM_NAME:-runtime_hmd_pose}" # runtime HMD pose SHM object name
RUNTIME_HAND_STREAM="${RUNTIME_HAND_STREAM:-runtime_hand_tracking}" # runtime hand output stream name
RUNTIME_HAND_SHM_NAME="${RUNTIME_HAND_SHM_NAME:-runtime_hand_tracking}" # runtime hand SHM object name

VIDEO_INPUT="${VIDEO_INPUT:-shm}"   # stereo video input transport;  none, shm, tcp
VIDEO_REGISTRY="${VIDEO_REGISTRY:-/tmp/xr_video_streams.json}" # stereo video source registry path
VIDEO_STREAM="${VIDEO_STREAM:-stereo_video}" # stereo video source stream name
VIDEO_TCP_HOST="${VIDEO_TCP_HOST:-127.0.0.1}" # TCP host for video input
VIDEO_TCP_PORT="${VIDEO_TCP_PORT:-45700}" # TCP port for video input
PUBLISH_RUNTIME_VIDEO="${PUBLISH_RUNTIME_VIDEO:-1}" # publish runtime stereo video SHM output
RUNTIME_VIDEO_REGISTRY="${RUNTIME_VIDEO_REGISTRY:-/tmp/runtime_video_streams.json}" # runtime video registry path
RUNTIME_VIDEO_STREAM="${RUNTIME_VIDEO_STREAM:-runtime_stereo_video}" # runtime video stream name
RUNTIME_VIDEO_SHM_NAME="${RUNTIME_VIDEO_SHM_NAME:-runtime_stereo_video}" # runtime video SHM object name

[[ -x "$XR_RUNTIME_ADAPTER_BIN" ]] || {
  echo "[ERROR] xr_runtime_adapter binary not found: $XR_RUNTIME_ADAPTER_BIN" >&2
  echo "[ERROR] Run: $ROOT_PROJECT/runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh" >&2
  exit 1
}

export RUNTIME_BODY_TRACKER_MAX_JUMP_M
export RUNTIME_CONTROLLER_MOVEMENT_SPACE
args=(
  --adapter "$ADAPTER"
  --mode "$MODE"
  --input "$INPUT"
  --registry "$TRACKING_REGISTRY"
  --tracking-transform-config "$TRACKING_TRANSFORM_CONFIG"
  --hmd-stream "$HMD_STREAM"
  --hand-stream "$HAND_STREAM"
  --hmd-3dof-registry "$HMD_3DOF_REGISTRY"
  --hmd-3dof-stream "$HMD_3DOF_STREAM"
  --hmd-3dof-reattach-on-stale-ms "$HMD_3DOF_REATTACH_ON_STALE_MS"
  --runtime-hmd-pose-stability-window-ms "$RUNTIME_HMD_POSE_STABILITY_WINDOW_MS"
  --runtime-hmd-pose-stability-max-distance-cm "$RUNTIME_HMD_POSE_STABILITY_MAX_DISTANCE_CM"
  --body-trackers-input "$BODY_TRACKERS_INPUT"
  --body-trackers-registry "$BODY_TRACKERS_REGISTRY"
  --body-trackers-stream "$BODY_TRACKERS_STREAM"
  --body-trackers-udp-bind-host "$BODY_TRACKERS_UDP_BIND_HOST"
  --body-trackers-udp-bind-port "$BODY_TRACKERS_UDP_BIND_PORT"
  --body-trackers-reattach-on-stale-ms "$BODY_TRACKERS_REATTACH_ON_STALE_MS"
  --runtime-body-tracker-hold-lost-ms "$RUNTIME_BODY_TRACKER_HOLD_LOST_MS"
  --runtime-body-tracker-predict-lost-ms "$RUNTIME_BODY_TRACKER_PREDICT_LOST_MS"
  --runtime-body-tracker-max-prediction-velocity-mps "$RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS"
  --runtime-body-tracker-max-prediction-acceleration-mps2 "$RUNTIME_BODY_TRACKER_MAX_PREDICTION_ACCELERATION_MPS2"
  --runtime-body-tracker-prediction-damping "$RUNTIME_BODY_TRACKER_PREDICTION_DAMPING"
  --runtime-body-tracker-max-prediction-path-m "$RUNTIME_BODY_TRACKER_MAX_PREDICTION_PATH_M"
  --runtime-body-tracker-max-distance-m "$RUNTIME_BODY_TRACKER_MAX_DISTANCE_M"
  --runtime-body-tracker-distance-reference-axis "$RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_AXIS"
  --runtime-body-tracker-distance-reference-offset-m "$RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_OFFSET_M"
  --runtime-body-tracker-prediction-window-mode "$RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MODE"
  --runtime-body-tracker-prediction-window-ms "$RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MS"
  --runtime-body-tracker-publish-predicted-velocity "$RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY"
  --runtime-body-tracker-reacquire-blend-ms "$RUNTIME_BODY_TRACKER_REACQUIRE_BLEND_MS"
  --runtime-body-tracker-prediction-publish-hz "$RUNTIME_BODY_TRACKER_PREDICTION_PUBLISH_HZ"
  --runtime-body-tracker-predicted-status "$RUNTIME_BODY_TRACKER_PREDICTED_STATUS"
  --spatial-proxy-mesh-input "$SPATIAL_PROXY_MESH_INPUT"
  --spatial-proxy-mesh-registry "$SPATIAL_PROXY_MESH_REGISTRY"
  --spatial-proxy-mesh-stream "$SPATIAL_PROXY_MESH_STREAM"
  --spatial-proxy-mesh-udp-bind-host "$SPATIAL_PROXY_MESH_UDP_BIND_HOST"
  --spatial-proxy-mesh-udp-bind-port "$SPATIAL_PROXY_MESH_UDP_BIND_PORT"
  --spatial-proxy-mesh-reattach-on-stale-ms "$SPATIAL_PROXY_MESH_REATTACH_ON_STALE_MS"
  --spatial-proxy-mesh-max-source-age-ms "$SPATIAL_PROXY_MESH_MAX_SOURCE_AGE_MS"
  --spatial-proxy-mesh-triangle-winding "$SPATIAL_PROXY_MESH_TRIANGLE_WINDING"
  --spatial-proxy-mesh-rotate-deg-x "$SPATIAL_PROXY_MESH_ROTATE_DEG_X"
  --spatial-proxy-mesh-rotate-deg-y "$SPATIAL_PROXY_MESH_ROTATE_DEG_Y"
  --spatial-proxy-mesh-rotate-deg-z "$SPATIAL_PROXY_MESH_ROTATE_DEG_Z"
  --hand-skeleton26-input "$HAND_SKELETON26_INPUT"
  --hand-skeleton26-registry "$HAND_SKELETON26_REGISTRY"
  --hand-skeleton26-stream "$HAND_SKELETON26_STREAM"
  --hand-skeleton26-tcp-host "$HAND_SKELETON26_TCP_HOST"
  --hand-skeleton26-tcp-port "$HAND_SKELETON26_TCP_PORT"
  --hand-skeleton26-derive-gestures "$HAND_SKELETON26_DERIVE_GESTURES"
  --runtime-derive-hand-gestures "$RUNTIME_DERIVE_HAND_GESTURES"
  --runtime-derive-hand-gestures-with-controller-input "$RUNTIME_DERIVE_HAND_GESTURES_WITH_CONTROLLER_INPUT"
  --runtime-derived-gestures-require-fresh-tracking "$RUNTIME_DERIVED_GESTURES_REQUIRE_FRESH_TRACKING"
  --runtime-derived-gesture-latch-ms "$RUNTIME_DERIVED_GESTURE_LATCH_MS"
  --runtime-ignore-backend-hand-gestures "$RUNTIME_IGNORE_BACKEND_HAND_GESTURES"
  --runtime-left-hand-gestures-enabled "$RUNTIME_LEFT_HAND_GESTURES_ENABLED"
  --runtime-right-hand-gestures-enabled "$RUNTIME_RIGHT_HAND_GESTURES_ENABLED"
  --runtime-derive-extra-gesture-buttons "$RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS"
  --runtime-derive-extra-gesture-buttons-with-controller-input "$RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS_WITH_CONTROLLER_INPUT"
  --derived-thumbs-up-button "$DERIVED_THUMBS_UP_BUTTON"
  --derived-index-point-button "$DERIVED_INDEX_POINT_BUTTON"
  --derived-thumbs-up-active-threshold "$DERIVED_THUMBS_UP_ACTIVE_THRESHOLD"
  --derived-index-point-active-threshold "$DERIVED_INDEX_POINT_ACTIVE_THRESHOLD"
  --derived-thumbs-up-deactive-threshold "$DERIVED_THUMBS_UP_DEACTIVE_THRESHOLD"
  --derived-index-point-deactive-threshold "$DERIVED_INDEX_POINT_DEACTIVE_THRESHOLD"
  --derived-extra-gesture-response-start "$DERIVED_EXTRA_GESTURE_RESPONSE_START"
  --derived-extra-gesture-hold-ms "$DERIVED_EXTRA_GESTURE_HOLD_MS"
  --derived-pinch-active-threshold "$DERIVED_PINCH_ACTIVE_THRESHOLD"
  --derived-grab-active-threshold "$DERIVED_GRAB_ACTIVE_THRESHOLD"
  --derived-pinch-deactive-threshold "$DERIVED_PINCH_DEACTIVE_THRESHOLD"
  --derived-grab-deactive-threshold "$DERIVED_GRAB_DEACTIVE_THRESHOLD"
  --derived-pinch-response-start "$DERIVED_PINCH_RESPONSE_START"
  --derived-grab-response-start "$DERIVED_GRAB_RESPONSE_START"
  --runtime-hand-gate-max-jump-m "$RUNTIME_HAND_GATE_MAX_JUMP_M"
  --runtime-hand-gate-confirm-frames "$RUNTIME_HAND_GATE_CONFIRM_FRAMES"
  --runtime-hand-gate-confirm-max-step-m "$RUNTIME_HAND_GATE_CONFIRM_MAX_STEP_M"
  --runtime-hand-gate-hold-lost-ms-hand-tracking "$RUNTIME_HAND_GATE_HOLD_LOST_MS_HAND_TRACKING"
  --runtime-hand-gate-hold-lost-ms-imu "$RUNTIME_HAND_GATE_HOLD_LOST_MS_IMU"
  --runtime-hand-gate-predict-lost-ms-hand-tracking "$RUNTIME_HAND_GATE_PREDICT_LOST_MS_HAND_TRACKING"
  --runtime-hand-gate-predict-lost-ms-imu "$RUNTIME_HAND_GATE_PREDICT_LOST_MS_IMU"
  --runtime-hand-gate-max-prediction-velocity-mps "$RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS"
  --runtime-hand-gate-prediction-damping "$RUNTIME_HAND_GATE_PREDICTION_DAMPING"
  --runtime-hand-tracking-prediction-window-mode "$RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MODE"
  --runtime-hand-tracking-prediction-window-ms "$RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MS"
  --runtime-hand-tracking-max-prediction-path-m "$RUNTIME_HAND_TRACKING_MAX_PREDICTION_PATH_M"
  --runtime-hand-tracking-max-distance-m "$RUNTIME_HAND_TRACKING_MAX_DISTANCE_M"
  --runtime-hand-tracking-distance-reference-axis "$RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_AXIS"
  --runtime-hand-tracking-distance-reference-offset-m "$RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_OFFSET_M"
  --runtime-hand-gate-publish-predicted-velocity "$RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY"
  --runtime-hand-gate-reacquire-blend-ms-hand-tracking "$RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_HAND_TRACKING"
  --runtime-hand-gate-reacquire-blend-ms-imu "$RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_IMU"
  --runtime-hand-gate-max-continuity-velocity-mps "$RUNTIME_HAND_GATE_MAX_CONTINUITY_VELOCITY_MPS"
  --runtime-jitter-filter-hmd-cm "$RUNTIME_JITTER_FILTER_HMD_CM"
  --runtime-jitter-filter-tracker-cm "$RUNTIME_JITTER_FILTER_TRACKER_CM"
  --runtime-jitter-filter-hmd-deg "$RUNTIME_JITTER_FILTER_HMD_DEG"
  --runtime-jitter-filter-tracker-deg-hand-tracking "$RUNTIME_JITTER_FILTER_TRACKER_DEG_HAND_TRACKING"
  --runtime-jitter-filter-tracker-deg-imu "$RUNTIME_JITTER_FILTER_TRACKER_DEG_IMU"
  --runtime-jitter-filter-hmd-vel-cm-s "$RUNTIME_JITTER_FILTER_HMD_VEL_CM_S"
  --runtime-jitter-filter-tracker-vel-cm-s "$RUNTIME_JITTER_FILTER_TRACKER_VEL_CM_S"
  --runtime-jitter-filter-hmd-ang-vel-deg-s "$RUNTIME_JITTER_FILTER_HMD_ANG_VEL_DEG_S"
  --runtime-jitter-filter-tracker-ang-vel-deg-s-hand-tracking "$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_HAND_TRACKING"
  --runtime-jitter-filter-tracker-ang-vel-deg-s-imu "$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_IMU"
  --runtime-jitter-filter-hmd-vel-smooth-alpha "$RUNTIME_JITTER_FILTER_HMD_VEL_SMOOTH_ALPHA"
  --runtime-jitter-filter-tracker-vel-smooth-alpha "$RUNTIME_JITTER_FILTER_TRACKER_VEL_SMOOTH_ALPHA"
  --runtime-jitter-filter-hmd-ang-vel-smooth-alpha "$RUNTIME_JITTER_FILTER_HMD_ANG_VEL_SMOOTH_ALPHA"
  --runtime-jitter-filter-tracker-ang-vel-smooth-alpha-hand-tracking "$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_HAND_TRACKING"
  --runtime-jitter-filter-tracker-ang-vel-smooth-alpha-imu "$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_IMU"
  --runtime-jitter-filter-hmd-pos-smooth-alpha "$RUNTIME_JITTER_FILTER_HMD_POS_SMOOTH_ALPHA"
  --runtime-jitter-filter-tracker-pos-smooth-alpha "$RUNTIME_JITTER_FILTER_TRACKER_POS_SMOOTH_ALPHA"
  --runtime-jitter-filter-hmd-rot-smooth-alpha "$RUNTIME_JITTER_FILTER_HMD_ROT_SMOOTH_ALPHA"
  --runtime-jitter-filter-tracker-rot-smooth-alpha-hand-tracking "$RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_HAND_TRACKING"
  --runtime-jitter-filter-tracker-rot-smooth-alpha-imu "$RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_IMU"
  --runtime-jitter-filter-body-tracker "$RUNTIME_JITTER_FILTER_BODY_TRACKER"
  --runtime-jitter-filter-body-tracker-cm "$RUNTIME_JITTER_FILTER_BODY_TRACKER_CM"
  --runtime-jitter-filter-body-tracker-deg "$RUNTIME_JITTER_FILTER_BODY_TRACKER_DEG"
  --runtime-jitter-filter-body-tracker-vel-cm-s "$RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_CM_S"
  --runtime-jitter-filter-body-tracker-ang-vel-deg-s "$RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_DEG_S"
  --runtime-jitter-filter-body-tracker-vel-smooth-alpha "$RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_SMOOTH_ALPHA"
  --runtime-jitter-filter-body-tracker-ang-vel-smooth-alpha "$RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_SMOOTH_ALPHA"
  --runtime-jitter-filter-body-tracker-pos-smooth-alpha "$RUNTIME_JITTER_FILTER_BODY_TRACKER_POS_SMOOTH_ALPHA"
  --runtime-jitter-filter-body-tracker-rot-smooth-alpha "$RUNTIME_JITTER_FILTER_BODY_TRACKER_ROT_SMOOTH_ALPHA"
  --controller-input-mode "$CONTROLLER_INPUT_MODE"
  --controller-input-transport "$CONTROLLER_INPUT_TRANSPORT"
  --controller-input-registry "$CONTROLLER_INPUT_REGISTRY"
  --controller-input-stream "$CONTROLLER_INPUT_STREAM"
  --controller-input-host "$CONTROLLER_INPUT_HOST"
  --controller-input-port "$CONTROLLER_INPUT_PORT"
  --controller-input-conflict-policy "$CONTROLLER_INPUT_CONFLICT_POLICY" \
  --controller-input-stale-policy "$CONTROLLER_INPUT_STALE_POLICY"
  --override-controller-block-gestures-while-stream-present "$OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT"
  --override-controller-gesture-block-latch-ms "$OVERRIDE_CONTROLLER_GESTURE_BLOCK_LATCH_MS"
  --max-controller-age-ms "$MAX_CONTROLLER_AGE_MS"
  --runtime-controller-lost-hand-pose-fallback "$RUNTIME_CONTROLLER_LOST_HAND_POSE_FALLBACK"
  --runtime-controller-left-orientation-source "$RUNTIME_CONTROLLER_LEFT_ORIENTATION_SOURCE"
  --runtime-controller-right-orientation-source "$RUNTIME_CONTROLLER_RIGHT_ORIENTATION_SOURCE"
  --hand-orientation-offset-only-runtime "$HAND_ORIENTATION_OFFSET_ONLY_RUNTIME"
  --runtime-controller-left-imu-acceleration-integration "$RUNTIME_CONTROLLER_LEFT_IMU_ACCELERATION_INTEGRATION"
  --runtime-controller-right-imu-acceleration-integration "$RUNTIME_CONTROLLER_RIGHT_IMU_ACCELERATION_INTEGRATION"
  --runtime-controller-left-imu-position-prediction "$RUNTIME_CONTROLLER_LEFT_IMU_POSITION_PREDICTION"
  --runtime-controller-right-imu-position-prediction "$RUNTIME_CONTROLLER_RIGHT_IMU_POSITION_PREDICTION"
  --runtime-controller-imu-prediction-window-mode "$RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MODE"
  --runtime-controller-imu-prediction-window-ms "$RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MS"
  --runtime-controller-imu-max-prediction-speed-mps "$RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS"
  --runtime-controller-imu-max-prediction-path-m "$RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_PATH_M"
  --runtime-controller-imu-max-distance-m "$RUNTIME_CONTROLLER_IMU_MAX_DISTANCE_M"
  --runtime-controller-imu-distance-reference-axis "$RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_AXIS"
  --runtime-controller-imu-distance-reference-offset-m "$RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_OFFSET_M"
  --runtime-controller-imu-lever-arm-mode "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_MODE"
  --runtime-controller-imu-lever-arm-left-x-m "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_X_M"
  --runtime-controller-imu-lever-arm-left-y-m "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Y_M"
  --runtime-controller-imu-lever-arm-left-z-m "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Z_M"
  --runtime-controller-imu-lever-arm-right-x-m "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_X_M"
  --runtime-controller-imu-lever-arm-right-y-m "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Y_M"
  --runtime-controller-imu-lever-arm-right-z-m "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Z_M"
  --runtime-controller-imu-lever-arm-centripetal-compensation "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_CENTRIPETAL_COMPENSATION"
  --runtime-controller-imu-lever-arm-tangential-compensation "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_TANGENTIAL_COMPENSATION"
  --runtime-controller-imu-lever-arm-angular-acceleration-smooth-alpha "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_ANGULAR_ACCELERATION_SMOOTH_ALPHA"
  --runtime-controller-imu-lever-arm-max-angular-acceleration-rad-s2 "$RUNTIME_CONTROLLER_IMU_LEVER_ARM_MAX_ANGULAR_ACCELERATION_RAD_S2"
  --runtime-controller-left-imu-yaw-correction "$RUNTIME_CONTROLLER_LEFT_IMU_YAW_CORRECTION"
  --runtime-controller-right-imu-yaw-correction "$RUNTIME_CONTROLLER_RIGHT_IMU_YAW_CORRECTION"
  --runtime-controller-imu-gravity-control-file "$XR_BACKEND_CONTROL_FILE"
  --runtime-controller-imu-control-poll-ms "$RUNTIME_CONTROLLER_IMU_CONTROL_POLL_MS"
  --runtime-controller-imu-acceleration-deadband-mps2 "$RUNTIME_CONTROLLER_IMU_ACCELERATION_DEADBAND_MPS2"
  --runtime-controller-imu-max-linear-acceleration-mps2 "$RUNTIME_CONTROLLER_IMU_MAX_LINEAR_ACCELERATION_MPS2"
  --runtime-controller-imu-yaw-correction-continuous "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS"
  --runtime-controller-imu-yaw-correction-on-reacquire "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE"
  --runtime-controller-imu-yaw-correction-deadband-deg "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_DEADBAND_DEG"
  --runtime-controller-imu-yaw-correction-blend-ms "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_BLEND_MS"
  --runtime-controller-imu-yaw-correction-trigger-filter "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_FILTER"
  --runtime-controller-imu-yaw-correction-trigger-hold-ms "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_HOLD_MS"
  --runtime-controller-imu-yaw-correction-trigger-max-range-deg "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_MAX_RANGE_DEG"
  --runtime-controller-imu-yaw-correction-interval-ms "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_INTERVAL_MS"
  --runtime-controller-imu-yaw-correction-reacquire-deadband-deg "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_DEADBAND_DEG"
  --runtime-controller-imu-yaw-correction-reacquire-blend-ms "$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_BLEND_MS"
  --runtime-controller-left-movement-space-hand-tracking "$RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_HAND_TRACKING"
  --runtime-controller-right-movement-space-hand-tracking "$RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_HAND_TRACKING"
  --runtime-controller-left-movement-space-imu "$RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_IMU"
  --runtime-controller-right-movement-space-imu "$RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_IMU"
  --controller-trigger-pinch-threshold "$CONTROLLER_TRIGGER_PINCH_THRESHOLD"
  --controller-grip-grab-threshold "$CONTROLLER_GRIP_GRAB_THRESHOLD"
  --tick-rate "$TICK_RATE"
  --prediction-ms "$PREDICTION_MS"
  --print-every "$PRINT_EVERY"
  --duration "$DURATION"
  --hmd-stale-policy hold_then_lost
  --hmd-hold-last-max-ms 100
  --hmd-reattach-on-stale-ms 500
  --hand-stale-policy lost #hold_then_lost
  --hand-hold-last-max-ms 300
  --hand-reattach-on-stale-ms 500
  --origin-mode "$ORIGIN_MODE"
  --runtime-frame runtime_local
  --publish-runtime-pose-shm
  --runtime-pose-registry "$RUNTIME_TRACKING_REGISTRY"
  --runtime-pose-stream "$RUNTIME_POSE_STREAM"
  --runtime-pose-shm-name "$RUNTIME_POSE_SHM_NAME"
  --publish-runtime-hand-shm
  --runtime-hand-registry "$RUNTIME_TRACKING_REGISTRY"
  --runtime-hand-stream "$RUNTIME_HAND_STREAM"
  --runtime-hand-shm-name "$RUNTIME_HAND_SHM_NAME"
  --video-input "$VIDEO_INPUT"
)

if [[ "$HMD_3DOF_PRIORITY" == "1" ]]; then
  args+=(--hmd-3dof-priority)
fi
if [[ "$RUNTIME_HMD_POSE_STABILITY_FILTER" == "1" ]]; then
  args+=(--runtime-hmd-pose-stability-filter)
fi
if [[ "$RUNTIME_HAND_STABILITY_GATE" == "1" ]]; then
  args+=(--runtime-hand-stability-gate)
fi
if [[ "$RUNTIME_JITTER_FILTER" == "1" ]]; then
  args+=(--runtime-jitter-filter)
fi
if [[ "$RUNTIME_BODY_TRACKER_STABILITY_GATE" == "1" ]]; then
  args+=(--runtime-body-tracker-stability-gate)
fi
if [[ -n "$RUNTIME_HAND_GATE_DEBUG_CSV" ]]; then
  args+=(--runtime-hand-gate-debug-csv "$RUNTIME_HAND_GATE_DEBUG_CSV")
fi

case "$VIDEO_INPUT" in
  none)
    ;;
  shm)
    args+=(--video-registry "$VIDEO_REGISTRY" --video-stream "$VIDEO_STREAM")
    ;;
  tcp)
    args+=(--video-tcp-host "$VIDEO_TCP_HOST" --video-tcp-port "$VIDEO_TCP_PORT")
    ;;
  *)
    echo "[ERROR] VIDEO_INPUT must be one of: none, shm, tcp; got: $VIDEO_INPUT" >&2
    exit 1
    ;;
esac

if [[ "$PUBLISH_RUNTIME_SPATIAL_PROXY_MESH" == "1" ]]; then
  args+=(
    --publish-runtime-spatial-proxy-mesh-shm
    --runtime-spatial-proxy-mesh-registry "$RUNTIME_SPATIAL_PROXY_MESH_REGISTRY"
    --runtime-spatial-proxy-mesh-stream "$RUNTIME_SPATIAL_PROXY_MESH_STREAM"
    --runtime-spatial-proxy-mesh-shm-name "$RUNTIME_SPATIAL_PROXY_MESH_SHM_NAME"
  )
fi

if [[ "$PUBLISH_RUNTIME_BODY_TRACKERS" == "1" ]]; then
  args+=(
    --publish-runtime-body-trackers-shm
    --runtime-body-trackers-registry "$RUNTIME_BODY_TRACKERS_REGISTRY"
    --runtime-body-trackers-stream "$RUNTIME_BODY_TRACKERS_STREAM"
    --runtime-body-trackers-shm-name "$RUNTIME_BODY_TRACKERS_SHM_NAME"
  )
fi

if [[ "$PUBLISH_RUNTIME_VIDEO" == "1" ]]; then
  args+=(
    --publish-runtime-video-shm
    --runtime-video-registry "$RUNTIME_VIDEO_REGISTRY"
    --runtime-video-stream "$RUNTIME_VIDEO_STREAM"
    --runtime-video-shm-name "$RUNTIME_VIDEO_SHM_NAME"
  )
fi

cat <<EOF2
ROOT_PROJECT=$ROOT_PROJECT # project root used to resolve binaries and configs
XR_RUNTIME_ADAPTER_BIN=$XR_RUNTIME_ADAPTER_BIN # xr_runtime_adapter executable path
TRACKING_REGISTRY=$TRACKING_REGISTRY # source tracking SHM registry path
RUNTIME_TRACKING_REGISTRY=$RUNTIME_TRACKING_REGISTRY # runtime-output tracking SHM registry path
TRACKING_TRANSFORM_CONFIG=$TRACKING_TRANSFORM_CONFIG # runtime coordinate/hand transform profile
ADAPTER=$ADAPTER # adapter backend mode used by xr_runtime_adapter
MODE=$MODE # main runtime loop mode
INPUT=$INPUT # primary HMD/hand input transport
HMD_STREAM=$HMD_STREAM # source HMD pose stream name
HMD_3DOF_PRIORITY=$HMD_3DOF_PRIORITY # prefer 3DoF HMD stream when available
HMD_3DOF_REGISTRY=$HMD_3DOF_REGISTRY # 3DoF source registry path
HMD_3DOF_STREAM=$HMD_3DOF_STREAM # 3DoF HMD pose stream name
TICK_RATE=$TICK_RATE # runtime publish/tick rate in Hz
PREDICTION_MS=$PREDICTION_MS # pose prediction horizon in milliseconds
VIDEO_INPUT=$VIDEO_INPUT # stereo video input transport
BODY_TRACKERS_INPUT=$BODY_TRACKERS_INPUT # body tracker input transport
SPATIAL_PROXY_MESH_INPUT=$SPATIAL_PROXY_MESH_INPUT # spatial proxy mesh input transport
SPATIAL_PROXY_MESH_SOURCE_REGISTRY=$SPATIAL_PROXY_MESH_SOURCE_REGISTRY # source registry for spatial proxy mesh input
SPATIAL_PROXY_MESH_SOURCE_STREAM=$SPATIAL_PROXY_MESH_SOURCE_STREAM # source stream for spatial proxy mesh input
SPATIAL_PROXY_MESH_TRIANGLE_WINDING=$SPATIAL_PROXY_MESH_TRIANGLE_WINDING # triangle winding policy for mesh output
SPATIAL_PROXY_MESH_ROTATE_DEG=($SPATIAL_PROXY_MESH_ROTATE_DEG_X,$SPATIAL_PROXY_MESH_ROTATE_DEG_Y,$SPATIAL_PROXY_MESH_ROTATE_DEG_Z)
SPATIAL_PROXY_MESH_UDP_BIND=$SPATIAL_PROXY_MESH_UDP_BIND_HOST:$SPATIAL_PROXY_MESH_UDP_BIND_PORT
PUBLISH_RUNTIME_SPATIAL_PROXY_MESH=$PUBLISH_RUNTIME_SPATIAL_PROXY_MESH # publish runtime spatial proxy mesh SHM output
RUNTIME_SPATIAL_PROXY_MESH_REGISTRY=$RUNTIME_SPATIAL_PROXY_MESH_REGISTRY # runtime spatial mesh registry path
RUNTIME_SPATIAL_PROXY_MESH_STREAM=$RUNTIME_SPATIAL_PROXY_MESH_STREAM # runtime spatial mesh stream name
RUNTIME_SPATIAL_PROXY_MESH_SHM_NAME=$RUNTIME_SPATIAL_PROXY_MESH_SHM_NAME # runtime spatial mesh SHM object name
PUBLISH_RUNTIME_BODY_TRACKERS=$PUBLISH_RUNTIME_BODY_TRACKERS # publish runtime body tracker SHM output
RUNTIME_BODY_TRACKER_STABILITY_GATE=$RUNTIME_BODY_TRACKER_STABILITY_GATE # enable runtime-side hold/prediction for body trackers
RUNTIME_BODY_TRACKER_HOLD_LOST_MS=$RUNTIME_BODY_TRACKER_HOLD_LOST_MS # hold last valid body tracker pose after loss
RUNTIME_BODY_TRACKER_PREDICT_LOST_MS=$RUNTIME_BODY_TRACKER_PREDICT_LOST_MS # predict body tracker pose after hold phase
RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS=$RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS # body tracker prediction velocity cap
RUNTIME_BODY_TRACKER_MAX_PREDICTION_ACCELERATION_MPS2=$RUNTIME_BODY_TRACKER_MAX_PREDICTION_ACCELERATION_MPS2 # body tracker prediction acceleration cap
RUNTIME_BODY_TRACKER_PREDICTION_DAMPING=$RUNTIME_BODY_TRACKER_PREDICTION_DAMPING # body tracker prediction damping
RUNTIME_BODY_TRACKER_MAX_PREDICTION_PATH_M=$RUNTIME_BODY_TRACKER_MAX_PREDICTION_PATH_M
RUNTIME_BODY_TRACKER_MAX_DISTANCE_M=$RUNTIME_BODY_TRACKER_MAX_DISTANCE_M
RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_AXIS=$RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_AXIS
RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_OFFSET_M=$RUNTIME_BODY_TRACKER_DISTANCE_REFERENCE_OFFSET_M
RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MODE=$RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MODE
RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MS=$RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MS
RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY=$RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY # publish decaying linear velocity during body tracker prediction
RUNTIME_BODY_TRACKER_REACQUIRE_BLEND_MS=$RUNTIME_BODY_TRACKER_REACQUIRE_BLEND_MS # blend from last predicted pose on in-window reacquire
RUNTIME_BODY_TRACKER_PREDICTION_PUBLISH_HZ=$RUNTIME_BODY_TRACKER_PREDICTION_PUBLISH_HZ # synthetic publish rate while body tracker source is stale
RUNTIME_BODY_TRACKER_PREDICTED_STATUS=$RUNTIME_BODY_TRACKER_PREDICTED_STATUS # predicted body tracker status
HAND_SKELETON26_INPUT=$HAND_SKELETON26_INPUT # 26-joint hand skeleton input transport
HAND_SKELETON26_DERIVE_GESTURES=$HAND_SKELETON26_DERIVE_GESTURES # derive gestures from skeleton26 input
RUNTIME_DERIVE_HAND_GESTURES=$RUNTIME_DERIVE_HAND_GESTURES # derive pinch/grab gestures in runtime adapter
RUNTIME_DERIVE_HAND_GESTURES_WITH_CONTROLLER_INPUT=$RUNTIME_DERIVE_HAND_GESTURES_WITH_CONTROLLER_INPUT # allow hand-derived gestures while controller input is active
RUNTIME_DERIVED_GESTURES_REQUIRE_FRESH_TRACKING=$RUNTIME_DERIVED_GESTURES_REQUIRE_FRESH_TRACKING # require fresh tracking before deriving gestures
RUNTIME_DERIVED_GESTURE_LATCH_MS=$RUNTIME_DERIVED_GESTURE_LATCH_MS # latch derived gestures across brief tracking gaps
RUNTIME_IGNORE_BACKEND_HAND_GESTURES=$RUNTIME_IGNORE_BACKEND_HAND_GESTURES # ignore backend-provided gesture fields and derive in runtime
RUNTIME_LEFT_HAND_GESTURES_ENABLED=$RUNTIME_LEFT_HAND_GESTURES_ENABLED # enable runtime gestures for left hand
RUNTIME_RIGHT_HAND_GESTURES_ENABLED=$RUNTIME_RIGHT_HAND_GESTURES_ENABLED # enable runtime gestures for right hand
RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS=$RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS # derive extra buttons from hand gestures
RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS_WITH_CONTROLLER_INPUT=$RUNTIME_DERIVE_EXTRA_GESTURE_BUTTONS_WITH_CONTROLLER_INPUT # allow extra derived buttons while controller input is active
DERIVED_THUMBS_UP_BUTTON=$DERIVED_THUMBS_UP_BUTTON # controller button mapped from thumbs-up gesture
DERIVED_INDEX_POINT_BUTTON=$DERIVED_INDEX_POINT_BUTTON # controller button mapped from index-point gesture
DERIVED_THUMBS_UP_ACTIVE_THRESHOLD=$DERIVED_THUMBS_UP_ACTIVE_THRESHOLD # activation threshold for thumbs-up gesture
DERIVED_INDEX_POINT_ACTIVE_THRESHOLD=$DERIVED_INDEX_POINT_ACTIVE_THRESHOLD # activation threshold for index-point gesture
DERIVED_THUMBS_UP_DEACTIVE_THRESHOLD=$DERIVED_THUMBS_UP_DEACTIVE_THRESHOLD # deactivation threshold for thumbs-up gesture
DERIVED_INDEX_POINT_DEACTIVE_THRESHOLD=$DERIVED_INDEX_POINT_DEACTIVE_THRESHOLD # deactivation threshold for index-point gesture
DERIVED_EXTRA_GESTURE_RESPONSE_START=$DERIVED_EXTRA_GESTURE_RESPONSE_START # response curve start for extra gesture buttons
DERIVED_EXTRA_GESTURE_HOLD_MS=$DERIVED_EXTRA_GESTURE_HOLD_MS # minimum hold time for extra gesture buttons
DERIVED_PINCH_ACTIVE_THRESHOLD=$DERIVED_PINCH_ACTIVE_THRESHOLD # activation threshold for derived pinch
DERIVED_GRAB_ACTIVE_THRESHOLD=$DERIVED_GRAB_ACTIVE_THRESHOLD # activation threshold for derived grab
DERIVED_PINCH_DEACTIVE_THRESHOLD=$DERIVED_PINCH_DEACTIVE_THRESHOLD # deactivation threshold for derived pinch
DERIVED_GRAB_DEACTIVE_THRESHOLD=$DERIVED_GRAB_DEACTIVE_THRESHOLD # deactivation threshold for derived grab
DERIVED_PINCH_RESPONSE_START=$DERIVED_PINCH_RESPONSE_START # response curve start for pinch
DERIVED_GRAB_RESPONSE_START=$DERIVED_GRAB_RESPONSE_START # response curve start for grab
RUNTIME_HMD_POSE_STABILITY_FILTER=$RUNTIME_HMD_POSE_STABILITY_FILTER # enable runtime HMD jump/speed filter
RUNTIME_HMD_POSE_STABILITY_WINDOW_MS=$RUNTIME_HMD_POSE_STABILITY_WINDOW_MS # HMD stability filter time window
RUNTIME_HMD_POSE_STABILITY_MAX_DISTANCE_CM=$RUNTIME_HMD_POSE_STABILITY_MAX_DISTANCE_CM # max HMD movement over stability window
RUNTIME_HAND_STABILITY_GATE=$RUNTIME_HAND_STABILITY_GATE # enable runtime hand pose stability gate
RUNTIME_HAND_GATE_MAX_JUMP_M=$RUNTIME_HAND_GATE_MAX_JUMP_M # max allowed hand jump before reacquire gating
RUNTIME_HAND_GATE_CONFIRM_FRAMES=$RUNTIME_HAND_GATE_CONFIRM_FRAMES # frames required to confirm reacquired hand
RUNTIME_HAND_GATE_CONFIRM_MAX_STEP_M=$RUNTIME_HAND_GATE_CONFIRM_MAX_STEP_M # max per-frame step while confirming reacquire
RUNTIME_HAND_GATE_HOLD_LOST_MS_HAND_TRACKING=$RUNTIME_HAND_GATE_HOLD_LOST_MS_HAND_TRACKING
RUNTIME_HAND_GATE_HOLD_LOST_MS_IMU=$RUNTIME_HAND_GATE_HOLD_LOST_MS_IMU
RUNTIME_HAND_GATE_PREDICT_LOST_MS_HAND_TRACKING=$RUNTIME_HAND_GATE_PREDICT_LOST_MS_HAND_TRACKING
RUNTIME_HAND_GATE_PREDICT_LOST_MS_IMU=$RUNTIME_HAND_GATE_PREDICT_LOST_MS_IMU
RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS=$RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS # cap prediction velocity for lost hands
RUNTIME_HAND_GATE_PREDICTION_DAMPING=$RUNTIME_HAND_GATE_PREDICTION_DAMPING # velocity damping during lost-hand prediction
RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MODE=$RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MODE
RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MS=$RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MS
RUNTIME_HAND_TRACKING_MAX_PREDICTION_PATH_M=$RUNTIME_HAND_TRACKING_MAX_PREDICTION_PATH_M
RUNTIME_HAND_TRACKING_MAX_DISTANCE_M=$RUNTIME_HAND_TRACKING_MAX_DISTANCE_M
RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_AXIS=$RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_AXIS
RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_OFFSET_M=$RUNTIME_HAND_TRACKING_DISTANCE_REFERENCE_OFFSET_M
RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY=$RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY # publish decaying linear velocity during hand prediction
RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_HAND_TRACKING=$RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_HAND_TRACKING
RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_IMU=$RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS_IMU
RUNTIME_HAND_GATE_MAX_CONTINUITY_VELOCITY_MPS=$RUNTIME_HAND_GATE_MAX_CONTINUITY_VELOCITY_MPS # max continuity velocity for hand gate
RUNTIME_HAND_GATE_DEBUG_CSV=$RUNTIME_HAND_GATE_DEBUG_CSV # optional CSV path for hand gate diagnostics
RUNTIME_JITTER_FILTER=$RUNTIME_JITTER_FILTER # enable runtime pose jitter deadband filter
RUNTIME_JITTER_FILTER_HMD_CM=$RUNTIME_JITTER_FILTER_HMD_CM # HMD position jitter deadband in cm
RUNTIME_JITTER_FILTER_TRACKER_CM=$RUNTIME_JITTER_FILTER_TRACKER_CM
RUNTIME_JITTER_FILTER_HMD_DEG=$RUNTIME_JITTER_FILTER_HMD_DEG # HMD orientation jitter deadband in degrees
RUNTIME_JITTER_FILTER_TRACKER_DEG_HAND_TRACKING=$RUNTIME_JITTER_FILTER_TRACKER_DEG_HAND_TRACKING
RUNTIME_JITTER_FILTER_TRACKER_DEG_IMU=$RUNTIME_JITTER_FILTER_TRACKER_DEG_IMU
RUNTIME_JITTER_FILTER_HMD_VEL_CM_S=$RUNTIME_JITTER_FILTER_HMD_VEL_CM_S # HMD linear velocity zero-deadband in cm/s
RUNTIME_JITTER_FILTER_TRACKER_VEL_CM_S=$RUNTIME_JITTER_FILTER_TRACKER_VEL_CM_S
RUNTIME_JITTER_FILTER_HMD_ANG_VEL_DEG_S=$RUNTIME_JITTER_FILTER_HMD_ANG_VEL_DEG_S # HMD angular velocity zero-deadband in deg/s
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_HAND_TRACKING=$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_HAND_TRACKING
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_IMU=$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_DEG_S_IMU
RUNTIME_JITTER_FILTER_HMD_VEL_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_HMD_VEL_SMOOTH_ALPHA # HMD linear velocity smoothing alpha
RUNTIME_JITTER_FILTER_TRACKER_VEL_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_TRACKER_VEL_SMOOTH_ALPHA
RUNTIME_JITTER_FILTER_HMD_ANG_VEL_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_HMD_ANG_VEL_SMOOTH_ALPHA # HMD angular velocity smoothing alpha
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_HAND_TRACKING=$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_HAND_TRACKING
RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_IMU=$RUNTIME_JITTER_FILTER_TRACKER_ANG_VEL_SMOOTH_ALPHA_IMU
RUNTIME_JITTER_FILTER_HMD_POS_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_HMD_POS_SMOOTH_ALPHA # HMD position smoothing alpha
RUNTIME_JITTER_FILTER_TRACKER_POS_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_TRACKER_POS_SMOOTH_ALPHA
RUNTIME_JITTER_FILTER_HMD_ROT_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_HMD_ROT_SMOOTH_ALPHA # HMD orientation smoothing alpha
RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_HAND_TRACKING=$RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_HAND_TRACKING
RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_IMU=$RUNTIME_JITTER_FILTER_TRACKER_ROT_SMOOTH_ALPHA_IMU
RUNTIME_JITTER_FILTER_BODY_TRACKER=$RUNTIME_JITTER_FILTER_BODY_TRACKER
RUNTIME_JITTER_FILTER_BODY_TRACKER_CM=$RUNTIME_JITTER_FILTER_BODY_TRACKER_CM
RUNTIME_JITTER_FILTER_BODY_TRACKER_DEG=$RUNTIME_JITTER_FILTER_BODY_TRACKER_DEG
RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_CM_S=$RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_CM_S
RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_DEG_S=$RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_DEG_S
RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_BODY_TRACKER_VEL_SMOOTH_ALPHA
RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_BODY_TRACKER_ANG_VEL_SMOOTH_ALPHA
RUNTIME_JITTER_FILTER_BODY_TRACKER_POS_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_BODY_TRACKER_POS_SMOOTH_ALPHA
RUNTIME_JITTER_FILTER_BODY_TRACKER_ROT_SMOOTH_ALPHA=$RUNTIME_JITTER_FILTER_BODY_TRACKER_ROT_SMOOTH_ALPHA
CONTROLLER_INPUT_MODE=$CONTROLLER_INPUT_MODE # controller/hand gesture merge mode
CONTROLLER_INPUT_TRANSPORT=$CONTROLLER_INPUT_TRANSPORT # controller input transport
OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT=$OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT # runtime-only gesture block gate
OVERRIDE_CONTROLLER_GESTURE_BLOCK_LATCH_MS=$OVERRIDE_CONTROLLER_GESTURE_BLOCK_LATCH_MS # runtime-only gesture block latch
MAX_CONTROLLER_AGE_MS=$MAX_CONTROLLER_AGE_MS # max age before controller_input is considered stale
RUNTIME_CONTROLLER_LOST_HAND_POSE_FALLBACK=$RUNTIME_CONTROLLER_LOST_HAND_POSE_FALLBACK # fallback pose policy when hand tracking is lost
RUNTIME_CONTROLLER_LEFT_ORIENTATION_SOURCE=$RUNTIME_CONTROLLER_LEFT_ORIENTATION_SOURCE
RUNTIME_CONTROLLER_RIGHT_ORIENTATION_SOURCE=$RUNTIME_CONTROLLER_RIGHT_ORIENTATION_SOURCE
HAND_ORIENTATION_OFFSET_ONLY_RUNTIME=$HAND_ORIENTATION_OFFSET_ONLY_RUNTIME
RUNTIME_CONTROLLER_LEFT_IMU_ACCELERATION_INTEGRATION=$RUNTIME_CONTROLLER_LEFT_IMU_ACCELERATION_INTEGRATION
RUNTIME_CONTROLLER_RIGHT_IMU_ACCELERATION_INTEGRATION=$RUNTIME_CONTROLLER_RIGHT_IMU_ACCELERATION_INTEGRATION
RUNTIME_CONTROLLER_LEFT_IMU_POSITION_PREDICTION=$RUNTIME_CONTROLLER_LEFT_IMU_POSITION_PREDICTION
RUNTIME_CONTROLLER_RIGHT_IMU_POSITION_PREDICTION=$RUNTIME_CONTROLLER_RIGHT_IMU_POSITION_PREDICTION
RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MODE=$RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MODE
RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MS=$RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MS
RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS=$RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS
RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_PATH_M=$RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_PATH_M
RUNTIME_CONTROLLER_IMU_MAX_DISTANCE_M=$RUNTIME_CONTROLLER_IMU_MAX_DISTANCE_M
RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_AXIS=$RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_AXIS
RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_OFFSET_M=$RUNTIME_CONTROLLER_IMU_DISTANCE_REFERENCE_OFFSET_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_MODE=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_MODE
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_X_M=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_X_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Y_M=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Y_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Z_M=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Z_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_X_M=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_X_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Y_M=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Y_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Z_M=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Z_M
RUNTIME_CONTROLLER_IMU_LEVER_ARM_CENTRIPETAL_COMPENSATION=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_CENTRIPETAL_COMPENSATION
RUNTIME_CONTROLLER_IMU_LEVER_ARM_TANGENTIAL_COMPENSATION=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_TANGENTIAL_COMPENSATION
RUNTIME_CONTROLLER_IMU_LEVER_ARM_ANGULAR_ACCELERATION_SMOOTH_ALPHA=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_ANGULAR_ACCELERATION_SMOOTH_ALPHA
RUNTIME_CONTROLLER_IMU_LEVER_ARM_MAX_ANGULAR_ACCELERATION_RAD_S2=$RUNTIME_CONTROLLER_IMU_LEVER_ARM_MAX_ANGULAR_ACCELERATION_RAD_S2
RUNTIME_CONTROLLER_LEFT_IMU_YAW_CORRECTION=$RUNTIME_CONTROLLER_LEFT_IMU_YAW_CORRECTION
RUNTIME_CONTROLLER_RIGHT_IMU_YAW_CORRECTION=$RUNTIME_CONTROLLER_RIGHT_IMU_YAW_CORRECTION
XR_BACKEND_CONTROL_FILE=$XR_BACKEND_CONTROL_FILE # controller IMU control source; reads gravity_magnitude and reset_counter
RUNTIME_CONTROLLER_IMU_CONTROL_POLL_MS=$RUNTIME_CONTROLLER_IMU_CONTROL_POLL_MS # backend control polling interval; 0 disables runtime polling
RUNTIME_CONTROLLER_IMU_ACCELERATION_DEADBAND_MPS2=$RUNTIME_CONTROLLER_IMU_ACCELERATION_DEADBAND_MPS2
RUNTIME_CONTROLLER_IMU_MAX_LINEAR_ACCELERATION_MPS2=$RUNTIME_CONTROLLER_IMU_MAX_LINEAR_ACCELERATION_MPS2
RUNTIME_CONTROLLER_IMU_PREDICTION_TIMING=RUNTIME_HAND_GATE_*_IMU
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_DEADBAND_DEG=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_DEADBAND_DEG
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_BLEND_MS=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_BLEND_MS
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_FILTER=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_FILTER
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_HOLD_MS=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_HOLD_MS
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_MAX_RANGE_DEG=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_TRIGGER_MAX_RANGE_DEG
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_INTERVAL_MS=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_INTERVAL_MS
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_DEADBAND_DEG=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_DEADBAND_DEG
RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_BLEND_MS=$RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_REACQUIRE_BLEND_MS
RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_HAND_TRACKING=$RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_HAND_TRACKING
RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_HAND_TRACKING=$RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_HAND_TRACKING
RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_IMU=$RUNTIME_CONTROLLER_LEFT_MOVEMENT_SPACE_IMU
RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_IMU=$RUNTIME_CONTROLLER_RIGHT_MOVEMENT_SPACE_IMU

PUBLISH_RUNTIME_VIDEO=$PUBLISH_RUNTIME_VIDEO # publish runtime stereo video SHM output
EOF2

exec "$XR_RUNTIME_ADAPTER_BIN" "${args[@]}" "$@"