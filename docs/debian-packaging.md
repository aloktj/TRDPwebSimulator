# Debian packaging via CPack

The project now ships a minimal **CPack** configuration that can produce a `.deb` from a CMake build tree.

## Build steps
1. Configure the project (example with a fresh build directory):
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   ```
2. Build the binaries and assets:
   ```bash
   cmake --build build
   ```
3. Generate the Debian package from the build tree:
   ```bash
   cmake --build build --target package
   # or
   (cd build && cpack -G DEB)
   ```

The resulting `.deb` will be placed in the `build/` directory.

### Building for Raspberry Pi
Raspberry Pi OS typically uses the `armhf` (32-bit) or `arm64` (64-bit) architectures. You can either build natively on a Pi or cross-build from another machine.

- **Native build on a Pi**: run the same commands as above on the device; CPack will auto-detect the architecture.
- **Cross-build**: set the architecture explicitly so the package metadata matches the Pi target:
  ```bash
  # Example for a 64-bit Pi target from an x86_64 host
  cmake -S . -B build-rpi -DCMAKE_BUILD_TYPE=Release -DCPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64 \
        -DCMAKE_TOOLCHAIN_FILE=/path/to/your/rpi/toolchain.cmake
  cmake --build build-rpi --target package
  ```
  Use `armhf` instead of `arm64` for 32-bit Raspberry Pi OS. You can also provide `DEB_TARGET_ARCH` in the environment instead of `CPACK_DEBIAN_PACKAGE_ARCHITECTURE`.

## Install layout
The CPack rules install the following payload:
- `trdp_web_simulator` executable to `${CMAKE_INSTALL_BINDIR}` (usually `/usr/bin`).
- Static UI assets under `${CMAKE_INSTALL_DATAROOTDIR}/trdpwebsimulator/static` (e.g., `/usr/share/trdpwebsimulator/static`).
- Sample XML configs under `${CMAKE_INSTALL_DATAROOTDIR}/trdpwebsimulator/configs`.
- The project `README.md` under the doc directory `${CMAKE_INSTALL_DOCDIR}`.

### Dependencies
CPack uses `dpkg-shlibdeps` (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`) to infer shared library dependencies from the built binaries. Ensure the TRDP/TAU and Drogon runtimes are discoverable on the build machine so the generated package records the correct requirements.
