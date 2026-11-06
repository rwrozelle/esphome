import esphome.codegen as cg
from esphome.core import CORE
from esphome.core.entity_helpers import (
    register_device_class,
    register_unit_of_measurement,
)
from esphome.types import ConfigType
from tests.testing_helpers import ComponentManifestOverride

# Entity domains the coap_server tests build against
DOMAINS = (
    "binary_sensor",
    "button",
    "lock",
    "number",
    "sensor",
    "switch",
    "text_sensor",
    "valve",
)


def override_manifest(manifest: ComponentManifestOverride) -> None:
    # The real to_code needs a full config with an ID, so emit only the defines the tests need
    async def to_code_testing(config: ConfigType) -> None:
        for domain in DOMAINS:
            CORE.testing_ensure_platform_registered(domain)
        cg.add_define("USE_COAP_SERVER")
        cg.add_define("USE_COAP_SERVER_PORT", 5683)
        cg.add_define("USE_COAP_SERVER_MAX_CLIENTS", 4)
        # Same value the socket tests set; the tests use both IPv4 and IPv6 peers
        cg.add_define("USE_NETWORK_IPV6", True)
        # The tests look these strings up by value in the generated tables
        register_device_class("temperature")
        cg.add_define("USE_ENTITY_DEVICE_CLASS")
        register_unit_of_measurement("°C")
        cg.add_define("USE_ENTITY_UNIT_OF_MEASUREMENT")

    manifest.to_code = to_code_testing
    manifest.dependencies = manifest.dependencies + ["socket", "network", *DOMAINS]
