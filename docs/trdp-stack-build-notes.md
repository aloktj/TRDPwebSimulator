# TRDP Stack Build Verification

This note captures an end-to-end attempt to build the simulator against the current [aloktj/TCNopen](https://github.com/aloktj/TCNopen) TRDP/TAU stack (built and installed from source on Ubuntu 24.04).

## Steps performed
1. Installed Drogon, jsoncpp, tinyxml2, MySQL, Redis, and yaml-cpp development packages from Ubuntu repositories.
2. Cloned `aloktj/TCNopen`, configured with the `linux-posix-release` preset, built it, and installed it to `/usr/local` (CMake package configs landed in `/usr/local/lib/cmake/{TRDP,TCOpenTRDP}`).
3. Configured this project with `USE_SYSTEM_TRDP=ON` and an explicit `CMAKE_PREFIX_PATH=/usr/local/lib/cmake;/usr/lib/cmake` so CMake would pick up the installed TRDP/TAU targets.

## Result
CMake locates Drogon, TRDP, and TAU, but the build fails in `trdp_engine.cpp` due to API drift between the simulator and the current TRDP headers:
- Types such as `TRDP_REQUEST_T` no longer exist.
- TAU functions now expect session handles and different parameter counts (for example `tau_init`, `tlc_openSession`, `tlc_configSession`, `tlc_updateSession`, `tlc_getInterval`, and `tlp_subscribe`).
- PD/MD callback structs expose `resultCode` instead of `result`.

The failure shows the simulator’s TRDP integration is pinned to an older TRDP/TAU API and needs refactoring or a matching legacy TRDP release to build successfully with a “real” stack.
