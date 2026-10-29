#!/usr/bin/env python3
"""Run a Linux build with IPv4/IPv6 sockets disabled, without sudo or namespaces.

The filter is inherited by compilers, CMake, Git, vcpkg and build scripts. Unix
sockets still work (e.g. ccache); a missed upstream download fails locally.
"""
import ctypes
import ctypes.util
import errno
import os
import socket
import sys


class ArgCompare(ctypes.Structure):
    _fields_ = [("arg", ctypes.c_uint), ("op", ctypes.c_int),
                ("datum_a", ctypes.c_uint64), ("datum_b", ctypes.c_uint64)]


def disable_network():
    name = ctypes.util.find_library("seccomp")
    if not name:
        raise RuntimeError("libseccomp2 missing; run install_build_deps_ubuntu.sh before offline builds")
    lib = ctypes.CDLL(name, use_errno=True)
    lib.seccomp_init.argtypes = [ctypes.c_uint32]
    lib.seccomp_init.restype = ctypes.c_void_p
    lib.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    lib.seccomp_syscall_resolve_name.restype = ctypes.c_int
    lib.seccomp_rule_add_array.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int,
                                          ctypes.c_uint, ctypes.POINTER(ArgCompare)]
    lib.seccomp_load.argtypes = [ctypes.c_void_p]
    lib.seccomp_release.argtypes = [ctypes.c_void_p]
    context = lib.seccomp_init(0x7FFF0000)  # SCMP_ACT_ALLOW
    if not context:
        raise RuntimeError("cannot initialize offline socket filter")
    try:
        syscall = lib.seccomp_syscall_resolve_name(b"socket")
        if syscall < 0:
            raise RuntimeError("socket syscall not supported by libseccomp")
        for family in (socket.AF_INET, socket.AF_INET6):
            condition = ArgCompare(0, 4, family, 0)  # SCMP_CMP_EQ
            result = lib.seccomp_rule_add_array(context, 0x50000 | errno.ENETUNREACH,
                                                syscall, 1, ctypes.byref(condition))
            if result != 0:
                raise RuntimeError(f"cannot configure offline socket filter: {result}")
        result = lib.seccomp_load(context)
        if result != 0:
            raise RuntimeError(f"cannot enable offline socket filter: {result}")
    finally:
        lib.seccomp_release(context)


if __name__ == "__main__":
    try:
        disable_network()
        os.execvp(sys.argv[1], sys.argv[1:])
    except (OSError, RuntimeError) as error:
        sys.exit(f"[dependencies][ERROR] {error}")
