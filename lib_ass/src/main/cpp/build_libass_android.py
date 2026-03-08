from abc import ABC, abstractmethod
import os
import shutil
import subprocess
import tarfile
from argparse import ArgumentParser
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path, PurePosixPath
from platform import system
from textwrap import dedent
from urllib import request
from urllib.parse import urlparse


@dataclass(frozen=True)
class Target:
    """Android ABI target and cross-compilation metadata.

    Attributes:
        abi: Android ABI name (e.g., "arm64-v8a", "armeabi-v7a"). From https://developer.android.com/ndk/guides/other_build_systems
        triple: LLVM target triple (e.g., "aarch64-linux-android"). From https://developer.android.com/ndk/guides/other_build_systems
        meson_cpu: Meson cpu value for host_machine.
        meson_cpu_family: Meson cpu_family value for host_machine. From https://mesonbuild.com/Reference-tables.html#cpu-families
    """

    abi: str  
    triple: str
    meson_cpu: str
    meson_cpu_family: str


class BuildSystem(Enum):
    """Build system used by a project."""

    MESON = auto()
    AUTOTOOLS = auto()


class ABCProjectDownload(ABC):
    """Abstract base for downloading and extracting project sources."""

    @abstractmethod
    def download_and_extract(self, build_dir: Path) -> Path:
        pass


class ProjectDownloadTar(ABCProjectDownload):
    """Downloads a project from a URL and extracts it from a tarball (.tar.gz, .tar.xz, etc.)."""

    def __init__(self, download_url: str):
        self.download_url = download_url

    def download_and_extract(self, build_dir: Path) -> Path:
        """Download the tarball and extract it to the build directory.

        Parameters:
            build_dir: Directory where the tarball will be downloaded and extracted.
                The project root is created as a subdirectory based on the archive name
                (e.g., libass-0.17.3.tar.xz becomes build_dir/libass-0.17.3).

        Returns:
            Path to the extracted project root directory.
        """
        url_file_path = PurePosixPath(urlparse(self.download_url).path).name  # Ex: libass-0.17.3.tar.xz
        url_file_name = url_file_path.rsplit(".", 2)[0] # Ex: libass-0.17.3

        project_dir = build_dir.joinpath(url_file_name)
        if not project_dir.is_dir():
            tar_path = build_dir.joinpath(url_file_path)

            print(f"Downloading {self.download_url}...")
            request.urlretrieve(self.download_url, tar_path)

            print(f"Extracting {tar_path}...")
            with tarfile.open(tar_path, "r:*") as tar:
                tar.extractall(path=build_dir)

        return project_dir


class ProjectDownloadGit(ABCProjectDownload):
    """Clones a project from a Git repository at a specific tag."""

    def __init__(self, git_repos_url: str, tag: str, recursive: bool):
        self.git_repos_url = git_repos_url
        self.tag = tag
        self.recursive = recursive

    def download_and_extract(self, build_dir: Path) -> Path:
        """Clone the repository (if not already present) into the build directory.

        Parameters:
            build_dir: Directory where the repository will be cloned.
                The project root is created as a subdirectory based on the repo name
                (e.g., libplacebo.git becomes build_dir/libplacebo).

        Returns:
            Path to the cloned project root directory.
        """
        project_file = PurePosixPath(urlparse(self.git_repos_url).path).name  # Ex: libplacebo.git
        project_name = project_file.rsplit(".", 1)[0] # Ex: libass-0.17.3

        project_dir = build_dir.joinpath(project_name)
        if not project_dir.is_dir():
            subprocess.run(["git", "clone", "--branch", self.tag, "--single-branch"] + (["--recursive"] if self.recursive else []) + [self.git_repos_url], cwd=build_dir, check=True)

        return project_dir


@dataclass
class Project:
    """Description of a dependency or project to build.

    Attributes:
        name: Project name (e.g., for lib path resolution).
        project_download: Strategy to obtain sources (tar or git).
        build_system: Build system used by the project.
        additional_build_flags: Flags passed to configure/meson for all ABIs.
        additional_build_flags_for_abi: Per-ABI flags (e.g., --enable-asm for x86_64).
    """

    name: str
    project_download: ABCProjectDownload
    build_system: BuildSystem
    additional_build_flags: list[str]
    additional_build_flags_for_abi: dict[Target, list[str]]
    is_shared: bool


@dataclass
class ToolchainEnvVar:
    """Variable for the NDK toolchain."""

    CC: str
    CXX: str
    AR: str
    AS: str
    LD: str
    NM: str
    RANLIB: str
    STRIP: str
    LDFLAGS: str
    PKG_CONFIG_SYSROOT_DIR: str
    PKG_CONFIG_LIBDIR: str


def create_meson_cross_file(target: Target, toolchain_env_var: ToolchainEnvVar, build_dir: Path, project: Project) -> Path:
    """Generate a Meson cross-compilation file for Android cross-builds.

    Parameters:
        target: Android ABI target (arm64-v8a, armeabi-v7a, x86, x86-64).
        toolchain_env_var: NDK toolchain paths.
        build_dir: Directory where the cross file is written.

    Returns:
        Path to the written Meson cross file.
    """
    if project.is_shared:
        default_library = "shared"
    else:
        default_library = "static"

    meson_cross_file_content = dedent(f"""
    [built-in options]
    buildtype = 'release'
    default_library = '{default_library}'
    wrap_mode = 'nodownload'
    c_link_args = '{toolchain_env_var.LDFLAGS}'
    cpp_link_args = '{toolchain_env_var.LDFLAGS}'

    [binaries]
    c = '{toolchain_env_var.CC}'
    cpp = '{toolchain_env_var.CXX}'
    ar = '{toolchain_env_var.AR}'
    as = '{toolchain_env_var.AS}'
    ld = '{toolchain_env_var.LD}'
    nm = '{toolchain_env_var.NM}'
    ranlib = '{toolchain_env_var.RANLIB}'
    strip = '{toolchain_env_var.STRIP}'
    pkg-config = 'pkg-config'

    [host_machine]
    system = 'android'
    cpu_family = '{target.meson_cpu_family}'
    cpu = '{target.meson_cpu}'
    endian = 'little'

    [properties]
    sys_root = '{toolchain_env_var.PKG_CONFIG_SYSROOT_DIR}'
    pkg_config_libdir = '{toolchain_env_var.PKG_CONFIG_LIBDIR}'
    """)

    meson_cross_file = build_dir.joinpath(f"{target.abi}.txt")
    meson_cross_file.write_text(meson_cross_file_content, encoding="utf-8")

    return meson_cross_file


def prepare_autotools_env(toolchain_env_var: ToolchainEnvVar) -> dict:
    """Build an environment dict with NDK toolchain variables for Autotools builds.

    Parameters:
        toolchain_env_var: NDK toolchain paths.

    Returns:
        A copy of the current environment with the toolchain variables set.
    """
    env = os.environ.copy()
    env.update({
        "CC": toolchain_env_var.CC,
        "CXX": toolchain_env_var.CXX,
        "AR": toolchain_env_var.AR,
        "AS": toolchain_env_var.AS,
        "LD": toolchain_env_var.LD,
        "NM": toolchain_env_var.NM,
        "RANLIB": toolchain_env_var.RANLIB,
        "STRIP": toolchain_env_var.STRIP,
        "LDFLAGS": toolchain_env_var.LDFLAGS,
        "PKG_CONFIG_SYSROOT_DIR": toolchain_env_var.PKG_CONFIG_SYSROOT_DIR,
        "PKG_CONFIG_LIBDIR": toolchain_env_var.PKG_CONFIG_LIBDIR,
        "DESTDIR": toolchain_env_var.PKG_CONFIG_SYSROOT_DIR
    })

    return env


def get_toolchain_path(ndk_path: Path) -> Path:
    """Resolve the NDK LLVM toolchain path for the current host OS.

    Parameters:
        ndk_path: Root path of the Android NDK installation.
            (e.g., .../ndk/27.0.12077973)

    Returns:
        Path to the prebuilt LLVM toolchain (toolchains/llvm/prebuilt/{os}).
        The OS variant is derived from the current platform:
        windows-x86_64, linux-x86_64, or darwin-x86_64.
    """
    system_name = system()

    # See NDK OS Variant in https://developer.android.com/ndk/guides/other_build_systems#overview
    if system_name == "Windows":
        # We don't really support Windows cause autotools doesn't work on Windows.
        os = "windows-x86_64"
    elif system_name == "Linux":
        os = "linux-x86_64"
    elif system_name == "Darwin":
        os = "darwin-x86_64"
    else:
        raise NotImplementedError(f"The system {system_name} isn't supported.")

    toolchain_path = ndk_path.joinpath("toolchains", "llvm", "prebuilt", os)
    if not toolchain_path.is_dir():
        raise NotADirectoryError(f"The toolchain \"{toolchain_path.absolute()}\" doesn't exist")

    return toolchain_path


def build_project(
    project: Project,
    target: Target,
    abi_version: int,
    toolchain_path: Path,
    build_dir: Path,
    jniLibs: Path,
) -> None:
    """Download and build a project for the given ABI target.

    Parameters:
        project: Project to build.
        target: Android ABI target (arm64-v8a, armeabi-v7a, x86, x86-64).
        abi_version: Minimum API level, used in compiler names
            (e.g., aarch64-linux-android33-clang).
        toolchain_path: Path to the NDK LLVM toolchain bin directory.
        build_dir: Root directory for builds.
        jniLibs: Intended output path for JNI libraries.
    """
    print(f"=== Building {project.name} for {target.abi} ===")

    target_dir = build_dir.joinpath(target.abi)
    target_dir.mkdir(exist_ok=True)

    toolchain_env_var = ToolchainEnvVar(
        str(toolchain_path.joinpath("bin", f'{target.triple}{abi_version}-clang')),
        str(toolchain_path.joinpath("bin", f'{target.triple}{abi_version}-clang++')),
        str(toolchain_path.joinpath("bin", "llvm-ar")),
        str(toolchain_path.joinpath("bin", "llvm-as")),
        str(toolchain_path.joinpath("bin", "ld.lld")),
        str(toolchain_path.joinpath("bin", "llvm-nm")),
        str(toolchain_path.joinpath("bin", "llvm-ranlib")),
        str(toolchain_path.joinpath("bin", "llvm-strip")),
        "-Wl,-z,max-page-size=16384", # Android require 16 KB page sizes: https://developer.android.com/guide/practices/page-sizes
        str(target_dir),
        str(target_dir.joinpath("usr", "local", "lib", "pkgconfig"))
    )

    project_dir = project.project_download.download_and_extract(build_dir)

    if project.build_system == BuildSystem.MESON:
        meson_cross_file = create_meson_cross_file(target, toolchain_env_var, build_dir, project)

        subprocess.run(
            [
                "meson",
                "setup",
                "build",
                "--cross-file", str(meson_cross_file)
            ] + project.additional_build_flags + project.additional_build_flags_for_abi.get(target, []),
            cwd=project_dir,
            check=True,
            encoding="utf-8"
        )
        subprocess.run(["meson", "compile", "-C", "build"], cwd=project_dir, check=True, encoding="utf-8")
        subprocess.run(["meson", "install", "-C", "build", "--destdir", toolchain_env_var.PKG_CONFIG_SYSROOT_DIR], cwd=project_dir, check=True, encoding="utf-8")

        shutil.rmtree(project_dir.joinpath("build"))
        meson_cross_file.unlink()
    elif project.build_system == BuildSystem.AUTOTOOLS:
        env_autotools = prepare_autotools_env(toolchain_env_var)

        if not project_dir.joinpath("configure").is_file():
            subprocess.run(["./autogen.sh"], cwd=project_dir, check=True, env=env_autotools, encoding="utf-8")
        
        if project.is_shared:
            default_library = ["--enable-shared", "--disable-static"]
        else:
            default_library = ["--enable-static", "--disable-shared"]

        subprocess.run(
            [
                "./configure",
                f"--host={target.triple}",
                "--with-pic",
            ] + project.additional_build_flags + project.additional_build_flags_for_abi.get(target, []) + default_library,
            cwd=project_dir,
            check=True,
            env=env_autotools,
            encoding="utf-8"
        )
        subprocess.run(["make"], cwd=project_dir, check=True, env=env_autotools, encoding="utf-8")
        subprocess.run(["make", "install"], cwd=project_dir, check=True, env=env_autotools, encoding="utf-8")
        subprocess.run(["make", "distclean"], cwd=project_dir, check=True, env=env_autotools, encoding="utf-8")
    
    library_extension = "so" if project.is_shared else "a"
    lib_path = Path(toolchain_env_var.PKG_CONFIG_SYSROOT_DIR).joinpath("usr", "local", "lib", f"lib{project.name}.{library_extension}")
    if not lib_path.is_file():
        raise FileNotFoundError(f"The file \"{lib_path}\" doesn't exist.")

    return lib_path



def main() -> None:
    parser = ArgumentParser(description="Build libass for android")
    parser.add_argument(
        "--ndk-path",
        type=Path,
        required=True,
        help="""
    The ndk path. Ex: C:\\Users\\moi15moi\\AppData\\Local\\Android\\Sdk\\ndk\\27.0.12077973
    """,
    )

    parser.add_argument(
        "--abi-version",
        type=int,
        required=True,
    )


    args = parser.parse_args()
    ndk_path: Path = args.ndk_path
    abi_version: int = args.abi_version

    if not ndk_path.is_dir():
        raise NotADirectoryError(f"The path you provided \"{ndk_path.absolute()}\" doesn't exist")
    
    toolchain_path = get_toolchain_path(ndk_path)

    python_file_dir = Path(__file__)

    build_dir = python_file_dir.parent.joinpath("build_native_lib")
    if build_dir.is_dir():
        shutil.rmtree(build_dir)
    build_dir.mkdir()

    target_arm = Target("armeabi-v7a", "armv7a-linux-androideabi", "armv7a", "arm")
    target_aarch64 = Target("arm64-v8a", "aarch64-linux-android", "aarch64", "aarch64")
    target_x86 = Target("x86", "i686-linux-android", "i686", "x86")
    target_x86_64 = Target("x86-64", "x86_64-linux-android", "x86_64", "x86_64")

    targets = [
        target_arm,
        target_aarch64,
        target_x86,
        target_x86_64
    ]

    projects = [
        Project("harfbuzz", ProjectDownloadTar("https://github.com/harfbuzz/harfbuzz/releases/download/11.3.3/harfbuzz-11.3.3.tar.xz"), BuildSystem.MESON, ["-Dtests=disabled", "-Ddocs=disabled", "-Dutilities=disabled"], {}, False),
        Project("freetype", ProjectDownloadTar("https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"), BuildSystem.AUTOTOOLS, ["--with-harfbuzz=yes", "--with-zlib=no"], {}, False),
        Project("fribidi", ProjectDownloadTar("https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz"), BuildSystem.MESON, ["-Ddocs=false", "-Dtests=false"], {}, False),
        Project("unibreak", ProjectDownloadTar("https://github.com/adah1972/libunibreak/releases/download/libunibreak_6_1/libunibreak-6.1.tar.gz"), BuildSystem.AUTOTOOLS, [], {}, False),
        Project("expat", ProjectDownloadTar("https://github.com/libexpat/libexpat/releases/download/R_2_7_1/expat-2.7.1.tar.xz"), BuildSystem.AUTOTOOLS, ["--without-tests", "--without-docbook"], {}, False),
        Project("fontconfig", ProjectDownloadTar("https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.16.0.tar.xz"), BuildSystem.MESON, ["-Dtests=disabled", "-Ddoc=disabled", "-Dtools=disabled", "-Dxml-backend=expat"], {}, False),
        Project("ass", ProjectDownloadTar("https://github.com/libass/libass/releases/download/0.17.3/libass-0.17.3.tar.xz"), BuildSystem.AUTOTOOLS, ["--enable-fontconfig", "--enable-libunibreak"], {target_aarch64: ["--enable-asm"], target_x86: ["--enable-asm"], target_x86_64: ["--enable-asm"]}, True),
    ]

    for target in targets:
        jniLibs = python_file_dir.parent.parent.joinpath("jniLibs", target.abi)

        if jniLibs.is_dir():
            shutil.rmtree(jniLibs)
        jniLibs.mkdir(parents=True)

        for project in projects:
            lib_path = build_project(project, target, abi_version, toolchain_path, build_dir, jniLibs)

            if project.name == "ass":
                shutil.copy(lib_path, jniLibs)

    include_dir = build_dir.joinpath("include")
    shutil.copytree(build_dir.joinpath(targets[0].abi, "usr", "local", "include"), include_dir)


if __name__ == "__main__":
    main()
