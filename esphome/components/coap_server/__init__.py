from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component
from esphome.components.openthread.const import CONF_DEVICE_TYPE, CONF_POLL_PERIOD
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MAX_CONNECTIONS,
    CONF_ON_CLIENT_CONNECTED,
    CONF_ON_CLIENT_DISCONNECTED,
    CONF_PORT,
)
from esphome.core import CORE
import esphome.final_validate as fv

from .const import (
    CONF_CLIENT_PING_INTERVAL,
    CONF_CLIENT_PING_RETRY,
    CONF_CLIENT_PING_TIMEOUT_RATIO,
    CONF_ID_CONTEXT,
    CONF_MASTER_SALT,
    CONF_MASTER_SECRET,
    CONF_OSCORE,
    CONF_RECIPIENT_ID,
    CONF_SENDER_ID,
    CONF_SERVER_PING_INTERVAL,
    CONF_SERVER_PING_RETRY,
    CONF_SERVER_PING_TIMEOUT_RATIO,
    CONF_SUBSCRIPTION_CONFIRM,
)

DOMAIN = "coap_server"
CODEOWNERS = ["@rwrozelle"]
DEPENDENCIES = ["openthread"]

coap_server_ns = cg.esphome_ns.namespace("coap_server")
CoapServer = coap_server_ns.class_("CoapServer", cg.Component, cg.Controller)


# Used for OSCORE
def _hex_bytes(value):
    """Validate a hex string and normalise to lowercase with no separators."""
    value = cv.string(value)
    clean = value.replace(" ", "").replace(":", "")
    if len(clean) % 2 != 0:
        raise cv.Invalid("Hex string must have an even number of characters")
    try:
        bytes.fromhex(clean)
    except ValueError as e:
        raise cv.Invalid(f"Invalid hex string: {e}") from e
    return clean.lower()


# Used for OSCORE
def _hex_bytes_nonempty(value):
    value = _hex_bytes(value)
    if len(value) == 0:
        raise cv.Invalid("Value must not be empty")
    return value


# Used for OSCORE
def _hex_to_bytes_list(hex_str: str) -> list[int]:
    if not hex_str:
        return []
    return [int(hex_str[i : i + 2], 16) for i in range(0, len(hex_str), 2)]


# Used for OSCORE
def _validate_oscore(config):
    if config[CONF_SENDER_ID] == config[CONF_RECIPIENT_ID]:
        raise cv.Invalid(
            f"'{CONF_SENDER_ID}' and '{CONF_RECIPIENT_ID}' must be different"
        )
    return config


# Used for OSCORE
OSCORE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_MASTER_SECRET): _hex_bytes_nonempty,
            cv.Optional(CONF_MASTER_SALT, default=""): _hex_bytes,
            cv.Required(CONF_SENDER_ID): _hex_bytes_nonempty,
            cv.Required(CONF_RECIPIENT_ID): _hex_bytes_nonempty,
            cv.Optional(CONF_ID_CONTEXT, default=""): _hex_bytes,
        }
    ),
    _validate_oscore,
)

CONFIG_SCHEMA = cv.All(
    cv.only_on_esp32,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CoapServer),
            cv.Optional(CONF_PORT, default=5683): cv.uint16_t,
            # Sent to Client in /info so they know how often to check if Server is alive
            cv.Optional(CONF_SERVER_PING_INTERVAL, default="60s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=20)),
            ),
            cv.Optional(CONF_SERVER_PING_TIMEOUT_RATIO, default=2.5): cv.All(
                cv.float_, cv.Range(min=0.5, max=5.0)
            ),
            cv.Optional(CONF_SERVER_PING_RETRY, default=1): cv.int_range(min=1, max=5),
            # Used by Server to check if client is still alive
            cv.Optional(CONF_CLIENT_PING_INTERVAL, default="60s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=60)),
            ),
            cv.Optional(CONF_CLIENT_PING_TIMEOUT_RATIO, default=2.5): cv.All(
                cv.float_, cv.Range(min=0.5, max=5.0)
            ),
            cv.Optional(CONF_CLIENT_PING_RETRY, default=1): cv.int_range(min=1, max=5),
            # Maximum allowed active connections, list is used for checking aliveness
            # does not block additional client from doing calls that don't involve observation
            cv.Optional(CONF_MAX_CONNECTIONS, default=1): cv.int_range(min=1, max=5),
            # Used to block types of subscriptions
            # False - only NonConfirm Subscription requests are allowed
            # True - only Confirm Subscription requests are allowed
            cv.Optional(CONF_SUBSCRIPTION_CONFIRM, default=False): cv.boolean,
            # Triggers
            cv.Optional(CONF_ON_CLIENT_CONNECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_CLIENT_DISCONNECTED): automation.validate_automation(
                single=True
            ),
            # Optional OSCORE, if setup then client must send calls including OSCORE credentials
            cv.Optional(CONF_OSCORE): OSCORE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_COAP_SERVER")
    add_idf_component(
        name="cbor",
        repo="https://github.com/rwrozelle/idf-extra-components",
        ref="cbor7",
        path="cbor",
    )

    # Track controller registration for StaticVector sizing
    CORE.register_controller()

    cg.add(var.set_server_ping_interval(config[CONF_SERVER_PING_INTERVAL]))
    cg.add(var.set_server_ping_timeout_ratio(config[CONF_SERVER_PING_TIMEOUT_RATIO]))
    cg.add(var.set_server_ping_retry(config[CONF_SERVER_PING_RETRY]))
    cg.add(var.set_client_ping_interval(config[CONF_CLIENT_PING_INTERVAL]))
    cg.add(var.set_client_ping_timeout_ratio(config[CONF_CLIENT_PING_TIMEOUT_RATIO]))
    cg.add(var.set_client_ping_retry(config[CONF_CLIENT_PING_RETRY]))
    cg.add_define("USE_COAP_SERVER_MAX_CLIENTS", config[CONF_MAX_CONNECTIONS])
    if CONF_ON_CLIENT_CONNECTED in config:
        await automation.build_callback_automation(
            var,
            "add_on_client_connected_callback",
            [(cg.std_string, "client_address")],
            config[CONF_ON_CLIENT_CONNECTED],
        )
    if CONF_ON_CLIENT_DISCONNECTED in config:
        await automation.build_callback_automation(
            var,
            "add_on_client_disconnected_callback",
            [(cg.std_string, "client_address")],
            config[CONF_ON_CLIENT_DISCONNECTED],
        )
    cg.add_define("USE_COAP_SERVER_PORT", config[CONF_PORT])
    cg.add(var.set_subscription_confirm(config[CONF_SUBSCRIPTION_CONFIRM]))

    if oscore := config.get(CONF_OSCORE):
        cg.add_define("USE_COAP_OSCORE")
        cg.add(
            var.set_oscore_master_secret(_hex_to_bytes_list(oscore[CONF_MASTER_SECRET]))
        )
        cg.add(var.set_oscore_master_salt(_hex_to_bytes_list(oscore[CONF_MASTER_SALT])))
        cg.add(var.set_oscore_sender_id(_hex_to_bytes_list(oscore[CONF_SENDER_ID])))
        cg.add(
            var.set_oscore_recipient_id(_hex_to_bytes_list(oscore[CONF_RECIPIENT_ID]))
        )
        cg.add(var.set_oscore_id_context(_hex_to_bytes_list(oscore[CONF_ID_CONTEXT])))


def _final_validate(config):
    full_config = fv.full_config.get()
    ot_config = full_config.get("openthread")
    if ot_config is None:
        return
    if ot_config.get(CONF_DEVICE_TYPE) != "MTD":
        return
    poll_period = ot_config.get(CONF_POLL_PERIOD)
    if not poll_period or poll_period.total_milliseconds <= 0:
        return

    for conf_key in (CONF_SERVER_PING_INTERVAL, CONF_CLIENT_PING_INTERVAL):
        if config[conf_key] < poll_period:
            raise cv.Invalid(
                f"'{conf_key}' ({config[conf_key]}) must be >= openthread "
                f"poll_period ({poll_period}) when device_type is MTD",
                path=[conf_key],
            )


FINAL_VALIDATE_SCHEMA = _final_validate
