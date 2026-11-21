# The default curl 7.29 headers lack RISC-V type definitions.
set(VCPKG_LIBCURL_DLSYM_UPDATED_HEADERS ON CACHE BOOL "Use architecture-independent curl headers")
