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


@dataclass
class Target:
    abi: str # From https://developer.android.com/ndk/guides/other_build_systems
    triple: str # From https://developer.android.com/ndk/guides/other_build_systems
    cpu: str
    cpu_family: str


class BuildSystem(Enum):
    MESON = auto()
    AUTOTOOLS = auto()


class ABCProjectDownload(ABC):

    @abstractmethod
    def download_and_extract(self, build_dir: Path) -> Path:
        pass


class ProjectDownloadTar(ABCProjectDownload):

    def __init__(self, download_url: str):
        self.download_url = download_url

    def download_and_extract(self, build_dir: Path) -> Path:
        url_file_path = PurePosixPath(urlparse(self.download_url).path).name # Ex: libass-0.17.3.tar.xz
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

    def __init__(self, git_repos_url: str, tag: str, recursive: bool):
        self.git_repos_url = git_repos_url
        self.tag = tag
        self.recursive = recursive

    def download_and_extract(self, build_dir: Path) -> Path:
        project_file = PurePosixPath(urlparse(self.git_repos_url).path).name # Ex: libplacebo.git
        project_name = project_file.rsplit(".", 1)[0] # Ex: libass-0.17.3

        project_dir = build_dir.joinpath(project_name)
        print()
        subprocess.run(["git", "clone", "--branch", self.tag, "--single-branch"] + (["--recursive"] if self.recursive else []) + [self.git_repos_url], cwd=build_dir, check=True)

        return project_dir



@dataclass
class Project:
    name: str
    project_download: ABCProjectDownload
    build_system: BuildSystem
    additional_build_flags: list[str]
    additional_build_flags_for_abi: dict[str, list[str]] # Key: ABI (ex: "arm64-v8a"). Value: List of flags


@dataclass
class ToolchainEnvVar:
    CC: str
    CXX: str
    AR: str
    AS: str
    LD: str
    NM: str
    RANLIB: str
    STRIP: str
    YASM: str
    LDFLAGS: str
    prefix: str
    PKG_CONFIG_PATH: str


def create_meson_cross_file(target: Target, toolchain_env_var: ToolchainEnvVar, build_dir: Path) -> Path:

    meson_cross_file_content = dedent(f"""
    [built-in options]
    buildtype = 'release'
    default_library = 'shared'
    wrap_mode = 'nodownload'
    c_link_args = '{toolchain_env_var.LDFLAGS}'
    cpp_link_args = '{toolchain_env_var.LDFLAGS}'
    prefix = '{toolchain_env_var.prefix}'
    pkg_config_path = '{toolchain_env_var.PKG_CONFIG_PATH}'

    [binaries]
    c = '{toolchain_env_var.CC}'
    cpp = '{toolchain_env_var.CXX}'
    ar = '{toolchain_env_var.AR}'
    as = '{toolchain_env_var.AS}'
    ld = '{toolchain_env_var.LD}'
    nm = '{toolchain_env_var.NM}'
    ranlib = '{toolchain_env_var.RANLIB}'
    strip = '{toolchain_env_var.STRIP}'
    yasm = '{toolchain_env_var.YASM}'
    pkg-config = 'pkg-config'

    [host_machine]
    system = 'android'
    cpu_family = '{target.cpu_family}'
    cpu = '{target.cpu}'
    endian = 'little'
    """)

    meson_cross_file = build_dir.joinpath(f"{target.abi}.txt")
    meson_cross_file.write_text(meson_cross_file_content, encoding="utf-8")

    return meson_cross_file


def prepare_autotools_env(toolchain_env_var: ToolchainEnvVar) -> dict:
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
        "YASM": toolchain_env_var.YASM,
        "LDFLAGS": toolchain_env_var.LDFLAGS,
        "PKG_CONFIG_PATH": toolchain_env_var.PKG_CONFIG_PATH,
    })

    return env


def get_toolchain_path(ndk_path: Path) -> Path:
    system_name = system()

    # See NDK OS Variant in https://developer.android.com/ndk/guides/other_build_systems#overview
    if system_name == "Windows":
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


def build_project(project: Project, target: Target, abi_version: int, toolchain_path: Path, build_dir: Path, jniLibs: Path):
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
        str(toolchain_path.joinpath("bin", "yasm")),
        "-Wl,-z,max-page-size=16384", # Android require 16 KB page sizes: https://developer.android.com/guide/practices/page-sizes
        str(target_dir),
        str(target_dir.joinpath("lib", "pkgconfig"))
    )

    env_autotools = prepare_autotools_env(toolchain_env_var)
    project_dir = project.project_download.download_and_extract(build_dir)

    if project.build_system == BuildSystem.MESON:
        meson_cross_file = create_meson_cross_file(target, toolchain_env_var, build_dir)

        subprocess.run(["meson", "setup", "build", "--cross-file", str(meson_cross_file)] + project.additional_build_flags + project.additional_build_flags_for_abi.get(target.abi, []), cwd=project_dir, check=True)
        subprocess.run(["ninja", "-C", "build"], cwd=project_dir, check=True)

        subprocess.run(["ninja", "-C", "build", "install"], cwd=project_dir, check=True)

        shutil.rmtree(project_dir.joinpath("build"))
        meson_cross_file.unlink()
    elif project.build_system == BuildSystem.AUTOTOOLS:
        if not project_dir.joinpath("configure").is_file():
            subprocess.run(["./autogen.sh"], cwd=project_dir, check=True, env=env_autotools)

        subprocess.run(["./configure", f"--host={target.triple}", "--enable-shared", "--disable-static", "--with-pic", f"--prefix={toolchain_env_var.prefix}"] + project.additional_build_flags + project.additional_build_flags_for_abi.get(target.abi, []), cwd=project_dir, check=True, env=env_autotools)
        subprocess.run(["make"], cwd=project_dir, check=True, env=env_autotools)

        subprocess.run(["make", "install"], cwd=project_dir, check=True, env=env_autotools)
        subprocess.run(["make", "distclean"], cwd=project_dir, check=True, env=env_autotools)
    
    #lib_path = Path(toolchain_env_var.prefix).joinpath("lib", f"lib{project.name}.so")
    #if not lib_path.is_file():
    #    raise FileNotFoundError(f"The file \"{lib_path}\" doesn't exist.")

    #shutil.copy(lib_path, jniLibs)


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

    targets = [
        Target("armeabi-v7a", "armv7a-linux-androideabi", "armv7a", "arm"),
        Target("arm64-v8a", "aarch64-linux-android", "aarch64", "aarch64"),
        Target("x86", "i686-linux-android", "i686", "x86"),
        Target("x86-64", "x86_64-linux-android", "x86_64", "x86_64"),
    ]

    projects = [
        Project("harfbuzz", ProjectDownloadTar("https://github.com/harfbuzz/harfbuzz/releases/download/11.3.3/harfbuzz-11.3.3.tar.xz"), BuildSystem.MESON, ["-Dtests=disabled", "-Ddocs=disabled", "-Dutilities=disabled"], {}),
        Project("freetype", ProjectDownloadTar("https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"), BuildSystem.AUTOTOOLS, ["--with-harfbuzz=yes", "--with-zlib=no"], {}),
        Project("fribidi", ProjectDownloadTar("https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz"), BuildSystem.MESON, ["-Ddocs=false", "-Dtests=false"], {}),
        Project("unibreak", ProjectDownloadTar("https://github.com/adah1972/libunibreak/releases/download/libunibreak_6_1/libunibreak-6.1.tar.gz"), BuildSystem.AUTOTOOLS, [], {}),
        Project("expat", ProjectDownloadTar("https://github.com/libexpat/libexpat/releases/download/R_2_7_1/expat-2.7.1.tar.xz"), BuildSystem.AUTOTOOLS, ["--without-tests", "--without-docbook"], {}),
        Project("fontconfig", ProjectDownloadTar("https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.16.0.tar.xz"), BuildSystem.MESON, ["-Dtests=disabled", "-Ddoc=disabled", "-Dtools=disabled", "-Dxml-backend=expat"], {}),
        Project("ass", ProjectDownloadTar("https://github.com/libass/libass/releases/download/0.17.3/libass-0.17.3.tar.xz"), BuildSystem.AUTOTOOLS, ["--enable-fontconfig", "--enable-libunibreak"], {"arm64-v8a": ["--enable-asm"], "x86": ["--enable-asm"], "x86-64": ["--enable-asm"]}),
        Project("placebo", ProjectDownloadGit("https://code.videolan.org/videolan/libplacebo.git", "v7.351.0", True), BuildSystem.MESON, ["-Dopengl=enabled", "-Ddemos=false"], {}),
    ]

    for target in targets:
        jniLibs = python_file_dir.parent.parent.joinpath("jniLibs", target.abi)
        if jniLibs.is_dir():
            shutil.rmtree(jniLibs)
        jniLibs.mkdir(parents=True)

        for project in projects:
            build_project(project, target, abi_version, toolchain_path, build_dir, jniLibs)

    #include_dir = build_dir.joinpath("include")
    #shutil.copytree(build_dir.joinpath(targets[0].abi, "include"), include_dir)


if __name__ == "__main__":
    main()
