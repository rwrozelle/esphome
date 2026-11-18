"""Tests for the Zephyr toolchain code generation."""

import asyncio
from collections.abc import Iterator
from dataclasses import replace
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from esphome.components.stm32.zephyr_variants import stm32c5
from esphome.components.zephyr import (
    run_compile,
    zephyr_additional_framework_projects,
    zephyr_data,
    zephyr_toolchain_to_code,
)
from esphome.components.zephyr.const import KEY_PRJ_CONF
from esphome.components.zephyr.variants import ZEPHYR_MAINLINE
from esphome.const import CONF_FRAMEWORK, KEY_CORE
from esphome.core import CORE, EsphomeError


@pytest.fixture(autouse=True)
def stm32c5_config() -> None:
    CORE.data[KEY_CORE] = {}
    stm32c5.config_schema({CONF_FRAMEWORK: {}})


@pytest.fixture
def install() -> Iterator[MagicMock]:
    with patch("esphome.components.zephyr.framework.check_and_install") as mock:
        yield mock


def _board(valid: bool = True, revision: bool | None = None) -> object:
    return patch.multiple(
        "esphome.components.zephyr.dts_lookup",
        validate_board=MagicMock(return_value=valid),
        validate_board_revision=MagicMock(return_value=revision),
        get_rng_node_label=MagicMock(return_value="rng"),
    )


def _prj_conf() -> dict[str, object]:
    return {
        name.removeprefix("CONFIG_"): value
        for name, (value, _) in zephyr_data()[KEY_PRJ_CONF][""].items()
    }


def test_additional_projects_are_the_family_hals_on_mainline() -> None:
    assert zephyr_additional_framework_projects(ZEPHYR_MAINLINE) == {"hal_stm32"}


def test_additional_projects_are_none_on_another_sdk() -> None:
    assert zephyr_additional_framework_projects(replace(ZEPHYR_MAINLINE)) == set()


def test_to_code_installs_then_runs_base_family_and_variant(
    install: MagicMock,
) -> None:
    with _board():
        asyncio.run(zephyr_toolchain_to_code({"board": "nucleo_c5a3zg"}))

    install.assert_called_once_with(ZEPHYR_MAINLINE, "v4.5.0", {"hal_stm32"})
    prj_conf = _prj_conf()
    assert prj_conf["STD_CPP20"] is True  # base
    assert prj_conf["MAIN_STACK_SIZE"] == 4096  # not the base's 2048
    assert prj_conf["FPU"] is True  # family
    assert prj_conf["REQUIRES_FULL_LIBCPP"] is True  # family
    assert prj_conf["REBOOT"] is True  # variant
    assert "-DUSE_ZEPHYR" in CORE.build_flags


@pytest.mark.parametrize(
    ("valid", "revision", "message"),
    [
        pytest.param(False, None, "was not found", id="unknown_board"),
        pytest.param(True, False, "revision that is not valid", id="bad_revision"),
    ],
)
def test_to_code_rejects_the_board(
    install: MagicMock, valid: bool, revision: bool | None, message: str
) -> None:
    with _board(valid, revision), pytest.raises(EsphomeError, match=message):
        asyncio.run(zephyr_toolchain_to_code({}))
    # The board is looked up in the installed workspace
    install.assert_called_once()


def test_to_code_leaves_the_entropy_source_to_the_variant(install: MagicMock) -> None:
    with (
        _board(),
        patch(
            "esphome.components.zephyr.dts_lookup.get_rng_node_label", return_value=None
        ),
    ):
        asyncio.run(zephyr_toolchain_to_code({"board": "nucleo_c5a3zg"}))

    assert "ENTROPY_GENERATOR" not in _prj_conf()
    assert _prj_conf()["TEST_RANDOM_GENERATOR"] is True


@pytest.fixture
def build(tmp_path: Path) -> Iterator[MagicMock]:
    """run_compile with the install, the toolchain and west mocked."""
    CORE.config_path = tmp_path / "test.yaml"
    CORE.build_path = tmp_path / "build"
    CORE.name = "test"
    zephyr_data()["board"] = "xiao_stm32c5"
    framework_path = tmp_path / "ws"
    with (
        patch.multiple(
            "esphome.components.zephyr.framework",
            check_and_install=MagicMock(),
            get_build_paths=MagicMock(return_value=(Path("/py"), framework_path)),
            install_toolchain=MagicMock(return_value=Path("/sdk")),
        ),
        patch("esphome.framework_helpers.run_command_ok", return_value=True) as run,
    ):
        yield run


def test_run_compile_builds_with_sysbuild(build: MagicMock, tmp_path: Path) -> None:
    assert run_compile(None, {}) is True

    cmd = build.call_args.args[0]
    assert cmd[cmd.index("-b") + 1] == "xiao_stm32c5"
    assert "--sysbuild" in cmd
    assert cmd[cmd.index("-d") + 1] == str(tmp_path / "build" / ".west_build")
    assert build.call_args.kwargs["cwd"] == str(tmp_path / "ws")
    assert build.call_args.kwargs["env"]["ZEPHYR_SDK_INSTALL_DIR"] == str(Path("/sdk"))
    assert (tmp_path / "build" / "zephyr" / "CMakeLists.txt").is_file()


def test_run_compile_fails_with_west(build: MagicMock) -> None:
    build.return_value = False
    with pytest.raises(EsphomeError, match="Zephyr native build failed"):
        run_compile(None, {})


def test_run_compile_needs_the_variant_toolchain(build: MagicMock) -> None:
    with (
        patch.object(stm32c5.VARIANT, "toolchain", None),
        pytest.raises(EsphomeError, match="names no Zephyr SDK toolchain"),
    ):
        run_compile(None, {})
    build.assert_not_called()
