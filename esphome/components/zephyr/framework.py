"""Install the Zephyr framework for Zephyr toolchain builds."""

import os
from pathlib import Path
import time

from esphome.build_helpers.tools_cache import ZEPHYR_TOOLS_CACHE, tools_cache_path
from esphome.const import KEY_CORE, KEY_FRAMEWORK_VERSION
from esphome.core import CORE, EsphomeError
from esphome.framework_helpers import get_python_env_executable_path, install_lock

from .build_zephyr import add_ccache_env
from .framework_helpers import (
    ensure_venv,
    ensure_zephyr_requirements,
    fetch_missing_west_projects,
    install_framework,
    patch_gen_defines_dts_path,
    patch_west_build_source_dir_relpath,
)
from .sdk_install import ensure_sdk
from .variants import ZephyrManifest

_REQUIREMENTS: Path = Path(__file__).parent / "requirements.txt"
# The SDK versions in use are unknown; a partial download untouched this long has no owner,
# so a download interrupted more than a day ago starts over
_STALE_DOWNLOAD_AGE: int = 24 * 60 * 60


def get_zephyr_tools_path() -> Path:
    # Machine-global (OS user cache dir) so all projects share one install;
    # see espidf.framework.get_idf_tools_path for the location rationale.
    return tools_cache_path(*ZEPHYR_TOOLS_CACHE)


def _get_python_env_path(version: str) -> Path:
    return get_zephyr_tools_path() / "penvs" / version


def _get_toolchain_path(sdk_version: str, toolchain: str) -> Path:
    return (
        get_zephyr_tools_path() / "toolchains" / f"zephyr-sdk-{sdk_version}-{toolchain}"
    )


def _get_framework_path(manifest: ZephyrManifest, version: str) -> Path:
    return (
        get_zephyr_tools_path()
        / "frameworks"
        / f"{manifest.workspace_prefix}-{version}"
    )


def build_workspace() -> tuple[ZephyrManifest, str]:
    """The manifest this build installs and the version its workspace is installed at;
    the install and every reader of the workspace take it from here."""
    # deferred: zephyr's __init__ may import this module
    from . import zephyr_variant_manifest

    return zephyr_variant_manifest(), f"v{CORE.data[KEY_CORE][KEY_FRAMEWORK_VERSION]}"


def get_zephyr_base() -> Path:
    """This build's workspace's zephyr/ folder, Zephyr's ZEPHYR_BASE."""
    return _get_framework_path(*build_workspace()) / "zephyr"


def get_build_paths(manifest: ZephyrManifest, version: str) -> tuple[Path, Path]:
    """The venv's python and the workspace a build runs west in."""
    return (
        get_python_env_executable_path(_get_python_env_path(version), "python"),
        _get_framework_path(manifest, version),
    )


def get_build_env(
    python_executable: Path, framework_path: Path, sdk_path: Path, ccache: str | None
) -> dict[str, str]:
    """The environment west builds in, as on nrf52."""
    env = os.environ.copy()
    env["PATH"] = str(python_executable.parent) + os.pathsep + env.get("PATH", "")
    env["ZEPHYR_BASE"] = str(framework_path / "zephyr")
    # The SDK sits in the esphome cache, off CMake's search path
    env["ZEPHYR_SDK_INSTALL_DIR"] = str(sdk_path)
    env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
    add_ccache_env(env, ccache, ZEPHYR_TOOLS_CACHE)
    return env


def check_and_install(
    manifest: ZephyrManifest, version: str, projects: frozenset[str]
) -> None:
    """Install the workspace with the ``projects`` the build needs beyond the
    manifest's required ones; a workspace only gains projects, since the builds of
    one version share it."""
    framework_path = _get_framework_path(manifest, version)
    with install_lock(get_zephyr_tools_path(), framework_path.name):
        _check_and_install(manifest, version, framework_path, projects)


def _check_and_install(
    manifest: ZephyrManifest,
    version: str,
    framework_path: Path,
    projects: frozenset[str],
) -> None:
    python_env_path = _get_python_env_path(version)
    env_python_path = get_python_env_executable_path(python_env_path, "python")
    # Every workspace of this version shares the venv
    with install_lock(get_zephyr_tools_path(), f"penv-{version}"):
        install_venv = ensure_venv(python_env_path, _REQUIREMENTS, version)

    sentinel = framework_path / ".ready"
    zephyr_reqs = framework_path / "zephyr" / "scripts" / "requirements.txt"
    wanted = set(projects | manifest.source.required)
    if not sentinel.exists() or not zephyr_reqs.exists():
        install_framework(
            env_python_path, framework_path, manifest.source, version, wanted
        )
        sentinel.touch()
    else:
        fetch_missing_west_projects(
            env_python_path, framework_path, manifest.source, version, wanted
        )
    # Every run: existing installs need these too, and they are no-ops once applied
    patch_gen_defines_dts_path(framework_path)
    patch_west_build_source_dir_relpath(framework_path)

    with install_lock(get_zephyr_tools_path(), f"penv-{version}"):
        # One marker per workspace, since workspaces share the penv
        ensure_zephyr_requirements(
            python_env_path,
            framework_path,
            install_venv,
            version,
            f".zephyr_reqs_ready-{framework_path.name}",
        )


def _read_sdk_version(framework_path: Path) -> str:
    sdk_version_file = framework_path / "zephyr" / "SDK_VERSION"
    try:
        sdk_version = sdk_version_file.read_text(encoding="utf-8").strip()
    except OSError as err:
        raise EsphomeError(
            f"Can't read the Zephyr SDK version from {sdk_version_file}: {err}"
        ) from err
    if not sdk_version:
        raise EsphomeError(f"{sdk_version_file} names no Zephyr SDK version")
    return sdk_version


def install_toolchain(framework_path: Path, toolchain: str) -> Path:
    """Install the SDK named by the workspace's ``zephyr/SDK_VERSION``; returns its folder."""
    sdk_version = _read_sdk_version(framework_path)
    sdk_path = _get_toolchain_path(sdk_version, toolchain)
    cutoff = time.time() - _STALE_DOWNLOAD_AGE

    def _keep_leftover(leftover: Path) -> bool:
        part = leftover.with_name(leftover.name.removesuffix(".meta"))
        for candidate in (part, part.with_name(f"{part.name}.meta")):
            try:
                if candidate.stat().st_mtime >= cutoff:
                    return True
            except FileNotFoundError:
                continue
        return False

    ensure_sdk(
        sdk_path,
        sdk_version,
        toolchain,
        keep_leftover=_keep_leftover,
        lock_root=get_zephyr_tools_path(),
    )
    return sdk_path
