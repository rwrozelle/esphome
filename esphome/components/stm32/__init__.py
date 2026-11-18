"""STMicroelectronics STM32 chips."""

from esphome.const import PLATFORM_STM32
from esphome.schema_extractors import SCHEMA_EXTRACT, schema_extractor
from esphome.types import ConfigType

AUTO_LOAD: list[str] = ["zephyr", "preferences"]
CODEOWNERS = ["@rwrozelle"]
DOMAIN = "stm32"
IS_TARGET_PLATFORM = True
# The variant families zephyr builds for this platform
ZEPHYR_FAMILIES: frozenset[str] = frozenset({"stm32"})


@schema_extractor("schema")
def CONFIG_SCHEMA(config: ConfigType) -> ConfigType:
    # deferred: zephyr's variants import this package
    from esphome.components.zephyr import zephyr_config_schema, zephyr_toolchain_schema

    if config is SCHEMA_EXTRACT:
        return zephyr_toolchain_schema(ZEPHYR_FAMILIES)
    return zephyr_config_schema(config, ZEPHYR_FAMILIES, PLATFORM_STM32)
