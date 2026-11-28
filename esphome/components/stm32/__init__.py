"""STMicroelectronics STM32 chips."""

from argparse import Namespace

from esphome.const import PLATFORM_STM32
from esphome.core import EsphomeError
from esphome.schema_extractors import SCHEMA_EXTRACT, schema_extractor
from esphome.types import ConfigType

AUTO_LOAD: list[str] = ["zephyr", "preferences"]
CODEOWNERS = ["@rwrozelle"]
DOMAIN = "stm32"
IS_TARGET_PLATFORM = True


@schema_extractor("schema")
def CONFIG_SCHEMA(config: ConfigType) -> ConfigType:
    from esphome.components.zephyr import zephyr_config_schema, zephyr_toolchain_schema

    from .zephyr_variants import VARIANT_MODULES

    if config is SCHEMA_EXTRACT:
        return zephyr_toolchain_schema(VARIANT_MODULES)
    return zephyr_config_schema(config, VARIANT_MODULES, PLATFORM_STM32)


def run_compile(args: Namespace, config: ConfigType) -> bool:
    # Until the Zephyr build lands; without this the build falls through to PlatformIO
    raise EsphomeError("Building stm32 firmware is not supported yet")
