import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, sensor, switch, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_KILOWATT_HOURS,
    UNIT_MINUTE,
)

DEPENDENCIES = ["uart", "climate"]
AUTO_LOAD = ["binary_sensor", "sensor", "switch", "text_sensor"]

CONF_BEEP_ENABLED = "beep_enabled"
CONF_DIAGNOSTIC_INTERVAL = "diagnostic_interval"

kaeltebringer_ns = cg.esphome_ns.namespace("kaeltebringer")
KaeltebringerClimate = kaeltebringer_ns.class_(
    "KaeltebringerClimate", cg.PollingComponent, climate.Climate, uart.UARTDevice
)
KaeltebringerSwitch = kaeltebringer_ns.class_("KaeltebringerSwitch", switch.Switch)

SensorType = kaeltebringer_ns.enum("SensorType", True)
BinarySensorType = kaeltebringer_ns.enum("BinarySensorType", True)
TextSensorType = kaeltebringer_ns.enum("TextSensorType", True)
SwitchType = kaeltebringer_ns.enum("SwitchType", True)

# The config key doubles as the C++ enum name (upper-cased).
SENSORS = {
    "room_temperature": {
        "unit_of_measurement": UNIT_CELSIUS,
        "device_class": DEVICE_CLASS_TEMPERATURE,
        "state_class": STATE_CLASS_MEASUREMENT,
        "accuracy_decimals": 1,
    },
    "outlet_temperature": {
        "unit_of_measurement": UNIT_CELSIUS,
        "device_class": DEVICE_CLASS_TEMPERATURE,
        "state_class": STATE_CLASS_MEASUREMENT,
        "accuracy_decimals": 1,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    },
    "target_temperature": {
        "unit_of_measurement": UNIT_CELSIUS,
        "device_class": DEVICE_CLASS_TEMPERATURE,
        "state_class": STATE_CLASS_MEASUREMENT,
        "accuracy_decimals": 1,
    },
    "energy": {
        "unit_of_measurement": UNIT_KILOWATT_HOURS,
        "device_class": DEVICE_CLASS_ENERGY,
        "state_class": STATE_CLASS_TOTAL_INCREASING,
        "accuracy_decimals": 3,
    },
    "compressor_ticks": {
        "unit_of_measurement": UNIT_MINUTE,
        "device_class": DEVICE_CLASS_DURATION,
        "state_class": STATE_CLASS_TOTAL_INCREASING,
        "accuracy_decimals": 0,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    },
    "on_timer_minutes": {
        "unit_of_measurement": UNIT_MINUTE,
        "device_class": DEVICE_CLASS_DURATION,
        "state_class": STATE_CLASS_MEASUREMENT,
        "accuracy_decimals": 0,
    },
    "off_timer_minutes": {
        "unit_of_measurement": UNIT_MINUTE,
        "device_class": DEVICE_CLASS_DURATION,
        "state_class": STATE_CLASS_MEASUREMENT,
        "accuracy_decimals": 0,
    },
}

BINARY_SENSORS = {
    "indoor_active": {"device_class": DEVICE_CLASS_RUNNING},
    "mute": {},
    "on_timer_armed": {},
    "off_timer_armed": {},
    "energy_available": {"entity_category": ENTITY_CATEGORY_DIAGNOSTIC},
}

TEXT_SENSORS = {
    "compressor_state": {},
    "compressor_state_raw": {
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
        "disabled_by_default": True,
    },
    "fan_speed": {},
}

SWITCHES = {"eco": "ECO", "health": "HEALTH", "display": "DISPLAY_PANEL", "heater_8c": "HEATER_8C"}

CONFIG_SCHEMA = (
    climate.climate_schema(KaeltebringerClimate)
    .extend(
        {
            cv.Optional(CONF_BEEP_ENABLED, default=True): cv.boolean,
            cv.Optional(
                CONF_DIAGNOSTIC_INTERVAL, default="5min"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend({cv.Optional(key): sensor.sensor_schema(**kwargs) for key, kwargs in SENSORS.items()})
    .extend(
        {
            cv.Optional(key): binary_sensor.binary_sensor_schema(**kwargs)
            for key, kwargs in BINARY_SENSORS.items()
        }
    )
    .extend(
        {
            cv.Optional(key): text_sensor.text_sensor_schema(**kwargs)
            for key, kwargs in TEXT_SENSORS.items()
        }
    )
    .extend({cv.Optional(key): switch.switch_schema(KaeltebringerSwitch) for key in SWITCHES})
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("60s"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    await climate.register_climate(var, config)
    cg.add(var.set_beep_enabled(config[CONF_BEEP_ENABLED]))
    cg.add(var.set_diagnostic_interval(config[CONF_DIAGNOSTIC_INTERVAL]))

    for key in SENSORS:
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(var.set_sensor(getattr(SensorType, key.upper()), entity))

    for key in BINARY_SENSORS:
        if key in config:
            entity = await binary_sensor.new_binary_sensor(config[key])
            cg.add(var.set_binary_sensor(getattr(BinarySensorType, key.upper()), entity))

    for key in TEXT_SENSORS:
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(var.set_text_sensor(getattr(TextSensorType, key.upper()), entity))

    for key, enum_name in SWITCHES.items():
        if key in config:
            entity = await switch.new_switch(config[key])
            cg.add(var.set_switch(getattr(SwitchType, enum_name), entity))
