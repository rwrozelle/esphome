"""Tests for the stm32: platform."""

import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from esphome.components import stm32
from esphome.components.zephyr import (
    zephyr_add_prj_conf,
    zephyr_data,
    zephyr_framework_type,
    zephyr_toolchain_schema,
    zephyr_variant_name,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_BOARD,
    CONF_FRAMEWORK,
    CONF_TOOLCHAIN,
    CONF_TYPE,
    CONF_VARIANT,
    CONF_VERSION,
    KEY_CORE,
    KEY_FRAMEWORK_VERSION,
    KEY_TARGET_PLATFORM,
    PLATFORM_STM32,
    Toolchain,
)
from esphome.core import CORE, CoroPriority
from esphome.schema_extractors import SCHEMA_EXTRACT


@pytest.fixture(autouse=True)
def core_data() -> None:
    CORE.data[KEY_CORE] = {}


@pytest.mark.parametrize("variant", ["STM32C5", "stm32c5"])
def test_config_schema_sets_up_the_variant(variant: str) -> None:
    config = stm32.CONFIG_SCHEMA({CONF_VARIANT: variant})

    assert config[CONF_VARIANT] == "STM32C5"
    assert config[CONF_BOARD] == "nucleo_c5a3zg"
    assert CORE.toolchain == Toolchain.ZEPHYR
    assert CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] == PLATFORM_STM32
    assert CORE.data[KEY_CORE][KEY_FRAMEWORK_VERSION] == cv.Version(4, 5, 0)
    assert zephyr_variant_name() == "STM32C5"
    assert zephyr_framework_type() == "zephyr"
    assert zephyr_data()[CONF_BOARD] == "nucleo_c5a3zg"


def test_config_schema_keeps_the_board_and_framework() -> None:
    config = stm32.CONFIG_SCHEMA(
        {
            CONF_VARIANT: "STM32C5",
            CONF_BOARD: "xiao_stm32c5",
            CONF_FRAMEWORK: {CONF_TYPE: "zephyr", CONF_VERSION: "recommended"},
        }
    )

    assert config[CONF_BOARD] == "xiao_stm32c5"
    assert zephyr_data()[CONF_BOARD] == "xiao_stm32c5"
    assert CORE.data[KEY_CORE][KEY_FRAMEWORK_VERSION] == cv.Version(4, 5, 0)


@pytest.mark.parametrize(
    ("config", "message", "path"),
    [
        ({}, "required key not provided", [CONF_VARIANT]),
        ({CONF_VARIANT: "STM32F9"}, "Unknown value 'STM32F9'", [CONF_VARIANT]),
        (
            {CONF_VARIANT: "STM32C5", CONF_BOARD: "b" * 128},
            "String is too long \\(128 bytes, max 127\\)",
            [CONF_BOARD],
        ),
        (
            {CONF_VARIANT: "STM32C5", CONF_TOOLCHAIN: "platformio"},
            "Unknown value 'platformio'",
            [CONF_TOOLCHAIN],
        ),
        (
            {CONF_VARIANT: "STM32C5", CONF_FRAMEWORK: {"log_level": "INFO"}},
            "extra keys not allowed",
            [CONF_FRAMEWORK, "log_level"],
        ),
        # Checked by the variant, which knows its SDKs
        (
            {CONF_VARIANT: "STM32C5", CONF_FRAMEWORK: {CONF_TYPE: "ncs"}},
            "Valid: \\['zephyr'\\]",
            [CONF_FRAMEWORK, CONF_TYPE],
        ),
        (
            {CONF_VARIANT: "STM32C5", CONF_FRAMEWORK: {CONF_VERSION: "4.4.0"}},
            "requires zephyr >= 4.5.0",
            [CONF_FRAMEWORK, CONF_VERSION],
        ),
    ],
)
def test_config_schema_rejects(
    config: dict[str, object], message: str, path: list[str]
) -> None:
    with pytest.raises(cv.Invalid, match=message) as err:
        stm32.CONFIG_SCHEMA(config)
    assert err.value.path == path


def test_config_schema_accepts_the_zephyr_toolchain_key() -> None:
    config = stm32.CONFIG_SCHEMA({CONF_VARIANT: "STM32C5", CONF_TOOLCHAIN: "zephyr"})

    assert config[CONF_TOOLCHAIN] == Toolchain.ZEPHYR
    assert CORE.toolchain == Toolchain.ZEPHYR


def test_config_schema_rejects_another_toolchain() -> None:
    CORE.toolchain = Toolchain.PLATFORMIO
    with pytest.raises(cv.Invalid, match="stm32"):
        stm32.CONFIG_SCHEMA({CONF_VARIANT: "STM32C5"})


def test_schema_extract_gives_the_platform_schema() -> None:
    schema = stm32.CONFIG_SCHEMA(SCHEMA_EXTRACT)

    assert isinstance(schema, cv.Schema)
    assert schema({CONF_VARIANT: "stm32c5"})[CONF_VARIANT] == "STM32C5"


def test_toolchain_schema_lists_only_the_families_variants() -> None:
    with pytest.raises(cv.Invalid, match="Unknown value"):
        zephyr_toolchain_schema(frozenset({"other"}))({CONF_VARIANT: "STM32C5"})


def test_copy_files_writes_the_zephyr_build_files(tmp_path: Path) -> None:
    CORE.config_path = tmp_path / "test.yaml"
    CORE.build_path = tmp_path
    CORE.name = "test"
    stm32.CONFIG_SCHEMA({CONF_VARIANT: "STM32C5"})
    zephyr_add_prj_conf("REBOOT", True)

    stm32.copy_files()

    assert (tmp_path / "zephyr" / "prj.conf").read_text() == "CONFIG_REBOOT=y\n"
    assert (tmp_path / "zephyr" / "app.overlay").is_file()


def test_to_code_runs_the_zephyr_code_generation_first() -> None:
    """At PLATFORM priority, so the workspace is installed before any devicetree read."""
    assert stm32.to_code.priority == float(CoroPriority.PLATFORM)
    with patch(
        "esphome.components.zephyr.zephyr_toolchain_to_code", new=AsyncMock()
    ) as to_code:
        asyncio.run(stm32.to_code({CONF_VARIANT: "STM32C5"}))
    to_code.assert_awaited_once_with({CONF_VARIANT: "STM32C5"})


def test_run_compile_claims_the_build() -> None:
    """True tells the compile command the platform built it."""
    with patch(
        "esphome.components.zephyr.run_compile", new=MagicMock(return_value=True)
    ) as run_compile:
        assert stm32.run_compile("args", {}) is True
    run_compile.assert_called_once_with("args", {})
