include("${CMAKE_CURRENT_LIST_DIR}/riscv64-linux-riscv64-release.cmake")
# Match RVA23_MARCH in devices/common/linux/scripts/runtime/cpu_target.py.
set(VCPKG_C_FLAGS "-march=rv64gcbv_zic64b_zicbom_zicbop_zicboz_ziccamoa_ziccif_zicclsm_ziccrse_zicntr_zicond_zihintntl_zihintpause_zihpm_zimop_za64rs_zawrs_zfa_zfhmin_zcb_zcmop_zkt_zvbb_zvfhmin_zvkt_supm -mtune=rocket -g0")
set(VCPKG_CXX_FLAGS "${VCPKG_C_FLAGS}")
