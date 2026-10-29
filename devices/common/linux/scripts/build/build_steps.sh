#!/usr/bin/env bash
# Shared target selection for dependency preparation and package builds.

should_run_step() {
  local name="$1"
  local token

  if [[ -z "${XR_BUILD_ONLY:-}" ]]; then
    return 0
  fi

  for token in ${XR_BUILD_ONLY}; do
    case "$token" in
      all|everything) return 0 ;;
      "$name") return 0 ;;
      capture_service|capture_service_native|capture_cpp)
        [[ "$name" == "capture_service_cpp" ]] && return 0
        ;;
      drivers)
        case "$name" in
          openvr_driver|monado_driver) return 0 ;;
          xrizer) [[ "${XR_BUILD_XRIZER:-0}" == "1" ]] && return 0 ;;
        esac
        ;;
      steamvr|steam_vr)
        [[ "$name" == "openvr_driver" ]] && return 0
        ;;
      openxr)
        [[ "$name" == "monado_driver" ]] && return 0
        ;;
      gpl_tools|optional_gpl|drivers_with_xrizer|drivers_gpl)
        case "$name" in
          xrizer) return 0 ;;
          openvr_driver|monado_driver)
            [[ "$token" == drivers_with_xrizer || "$token" == drivers_gpl ]] && return 0
            ;;
        esac
        ;;
    esac
  done

  return 1
}


should_build_xrizer() {
  local token

  if [[ "${XR_BUILD_XRIZER:-0}" == "1" ]]; then
    return 0
  fi

  for token in ${XR_BUILD_ONLY:-}; do
    case "$token" in
      xrizer|gpl_tools|optional_gpl|drivers_with_xrizer|drivers_gpl)
        return 0
        ;;
    esac
  done

  return 1
}

should_download_hand_tracking_models() {
  local token

  if [[ "${XR_DOWNLOAD_HAND_TRACKING_MODELS:-0}" == "1" || "${XR_DOWNLOAD_MERCURY_MODELS:-0}" == "1" ]]; then
    return 0
  fi

  for token in ${XR_BUILD_ONLY:-}; do
    case "$token" in
      hand_tracking_models|mercury_models|hand_models|models)
        return 0
        ;;
    esac
  done

  return 1
}


vendor_components_enabled() {
  case "${XR_BUILD_VENDOR_COMPONENTS:-1}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}
