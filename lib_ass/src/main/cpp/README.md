# libass native build

`build_libass_android.py` cross-compiles libass and its dependencies for every Android ABI.

Gradle doesn't build these libraries. You must run the script before the first Gradle build (or Android Studio sync),
and again after updating a submodule in `third_party/`.

## Requirements

Linux or macOS with:
- `python3` (3.11 or newer)
- A JDK (17 or newer), used by Gradle to install the NDK
- `make`, `meson`, `ninja`, `pkg-config`, `autoconf`, `automake`, `libtool`, `gperf`, `gettext` and `nasm`

The NDK is installed automatically (the version comes from `gradle/libs.versions.toml`).

### Ubuntu
```sh
sudo apt-get update
sudo apt-get install -y make autoconf automake libtool gperf gettext nasm ninja-build pkg-config pipx
pipx ensurepath
pipx install meson
```
Fontconfig requires meson 1.11.0 or newer, which is more recent than the version available with `apt-get`.

### macOS
```sh
brew install autoconf automake libtool gperf gettext nasm meson ninja pkg-config
```

## Build

Run:
```sh
python3 lib_ass/src/main/cpp/build_libass_android.py --sdk-path <Android SDK path>
```

### Options

- `--reconfigure`: Run autogen, `meson setup` and `./configure` again for every project, even if they are already configured.
  Without it, they only run when a build directory isn't configured yet.
  Use it when the compiler (NDK version, minSdk) or a configure/meson option changes.
- `--debug`: Build the libraries without optimizations and with debug info (`-O0 -g`).
  Debug and release builds have separate build directories, so switching doesn't need `--reconfigure`.
  However, both install to the same output directory, so the last build is the one packaged in the app.

## Output

The output goes to `lib_ass/src/main/cpp/build_native_lib/`:
- `<abi>/lib/*.so`: the shared libraries of each ABI, imported by `CMakeLists.txt`.
- `include/`: the headers, exposed by the Prefab package `ass` (see `lib_ass/build.gradle.kts`).

The other directories in `build_native_lib/` are the build directories of each project, kept between runs so only the changed files are recompiled.
