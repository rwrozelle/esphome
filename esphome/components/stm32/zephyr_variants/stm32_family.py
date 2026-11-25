"""Code shared by the STM32 variants."""

from esphome.components.zephyr import zephyr_add_prj_conf
from esphome.types import ConfigType

FAMILY = "stm32"

# Vendor HAL west projects this family builds against on mainline Zephyr; the other
# vendor HALs are not fetched.
ADDITIONAL_FRAMEWORK_PROJECTS: frozenset[str] = frozenset({"hal_stm32"})


def to_code(config: ConfigType) -> None:
    """REQUIRES_FULL_LIBCPP selects the GNU C++ library; Zephyr's default minimal
    library has no STL, which ESPHome's headers need."""
    zephyr_add_prj_conf("CPP", True)
    zephyr_add_prj_conf("REQUIRES_FULL_LIBCPP", True)
    # Off by default, which leaves floating point in software
    zephyr_add_prj_conf("FPU", True)
