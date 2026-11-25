"""STMicroelectronics STM32 chips."""

from esphome.const import PLATFORM_STM32
from esphome.core import CoroPriority, coroutine_with_priority
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


@coroutine_with_priority(CoroPriority.PLATFORM)
async def to_code(config: ConfigType) -> None:
    # deferred: zephyr's variants import this package
    from esphome.components import zephyr

    await zephyr.zephyr_toolchain_to_code(config)


# Called by writer.py
def copy_files() -> None:
    # deferred: zephyr's variants import this package
    from esphome.components import zephyr

    zephyr.copy_files()


def run_compile(args: object, config: ConfigType) -> bool:
    # deferred: zephyr's variants import this package
    from esphome.components import zephyr

    return zephyr.run_compile(args, config)
