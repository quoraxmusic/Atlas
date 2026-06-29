# Building

Atlas builds on both macOS and Windows. macOS is the primary supported target, but Windows builds and a Windows installer are fully supported.

## Requirements

### Common

- CMake 3.22 or newer
- the JUCE folder next to this repo

The folder layout should look like this:

```text
Atlas/
  JUCE/
  Atlas/
```

### macOS

- macOS
- Xcode command line tools

### Windows

- Visual Studio with the Windows SDK
- Inno Setup (provides `ISCC.exe`) — only required to build the installer

## Release build

Use release builds for testing and release. They behave much better with CPU-heavy patches.

Atlas only builds AU and VST3 now. There is no standalone target or app bundle.

### macOS

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
```

The built files are placed under:

`build-release/Atlas_artefacts/Release`

### Windows

On Windows the build must be done in a single step, selecting the config at build
time:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All --config Release -j2
```

The built files are placed under:

`build-release/Atlas_artefacts/Release`

## Package installer

### macOS

After the release build:

```sh
./installer/build-pkg.sh
```

The package is written to:

`dist/`

Example:

`dist/Atlas-1.2.0.pkg`

If a package with that name already exists, the script adds a timestamp.

### Windows

Inno Setup is required to build the Windows installer. Run the configuration step
at least once, then build the `AtlasInstaller` target:

```sh
cmake --build build-release --target AtlasInstaller
```

If `ISCC.exe` is not found, the `AtlasInstaller` target is disabled — install Inno
Setup, make sure `ISCC.exe` is on `PATH`, and re-run CMake.

## Clean rebuild

```sh
rm -rf build-release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
```

On Windows, substitute the Windows build command above for the final step.

## Debug builds

Debug builds are useful for development only. Do not judge voice count or CPU from them.
