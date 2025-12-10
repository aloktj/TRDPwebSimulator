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

## Install layout
The CPack rules install the following payload:
- `trdp_web_simulator` executable to `${CMAKE_INSTALL_BINDIR}` (usually `/usr/bin`).
- Static UI assets under `${CMAKE_INSTALL_DATAROOTDIR}/trdpwebsimulator/static` (e.g., `/usr/share/trdpwebsimulator/static`).
- Sample XML configs under `${CMAKE_INSTALL_DATAROOTDIR}/trdpwebsimulator/configs`.
- The project `README.md` under the doc directory `${CMAKE_INSTALL_DOCDIR}`.

### Dependencies
CPack uses `dpkg-shlibdeps` (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`) to infer shared library dependencies from the built binaries. Ensure the TRDP/TAU and Drogon runtimes are discoverable on the build machine so the generated package records the correct requirements.
