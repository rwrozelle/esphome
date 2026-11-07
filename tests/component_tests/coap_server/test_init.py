"""Tests for the coap_server component."""

from collections.abc import Callable
from pathlib import Path
from unittest.mock import patch

import pytest

from esphome.components.coap_server import OT_BLOCKWISE_OPTION, _final_validate
from esphome.components.esp32 import KEY_VARIANT, VARIANT_ESP32C6
from esphome.components.openthread import OT_HEADER_DIR, OT_HEADER_FILE
import esphome.config_validation as cv
from esphome.const import PlatformFramework
from esphome.core import CORE
from tests.component_tests.types import SetCoreConfigCallable


def test_blockwise_core_option_added_on_esp32(
    tmp_path: Path,
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    config_path = tmp_path / "test.yaml"
    config_path.write_text(
        component_config_path("openthread.yaml").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    with patch("esphome.components.openthread.clean_build"):
        generate_main(config_path)
    header = CORE.relative_build_path(OT_HEADER_DIR, OT_HEADER_FILE)
    assert f"#define {OT_BLOCKWISE_OPTION} 1\n" in header.read_text(encoding="utf-8")


def test_blockwise_core_option_turned_off_rejected(
    set_core_config: SetCoreConfigCallable,
) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_VARIANT: VARIANT_ESP32C6},
        full_config={"openthread": {"core_options": {OT_BLOCKWISE_OPTION: "0"}}},
    )
    with pytest.raises(cv.Invalid, match=OT_BLOCKWISE_OPTION):
        _final_validate({})
