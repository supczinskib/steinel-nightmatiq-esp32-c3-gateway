import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    esp32_ble_tracker,
    number,
    select,
    sensor,
    text_sensor,
    web_server_base,
)
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32", "web_server", "esp32_ble_tracker"]
AUTO_LOAD = ["binary_sensor", "number", "select", "sensor", "text_sensor", "web_server_base"]

CONF_LUX_SENSOR_ID = "lux_sensor_id"
CONF_RSSI_SENSOR_ID = "rssi_sensor_id"
CONF_THRESHOLD_NUMBER_ID = "threshold_number_id"
CONF_MODE_SELECT_ID = "mode_select_id"
CONF_READY_BINARY_SENSOR_ID = "ready_binary_sensor_id"
CONF_ACTUAL_OUTPUT_BINARY_SENSOR_ID = "actual_output_binary_sensor_id"
CONF_STATUS_TEXT_SENSOR_ID = "status_text_sensor_id"
CONF_FIRMWARE_VERSION_TEXT_SENSOR_ID = "firmware_version_text_sensor_id"
CONF_HARDWARE_VERSION_SENSOR_ID = "hardware_version_sensor_id"
CONF_MANUFACTURER_TEXT_SENSOR_ID = "manufacturer_text_sensor_id"
CONF_COMPANY_ID_TEXT_SENSOR_ID = "company_id_text_sensor_id"
CONF_PRODUCT_ID_TEXT_SENSOR_ID = "product_id_text_sensor_id"
CONF_EXTENDED_DIAGNOSTICS = "extended_diagnostics"
CONF_WEB_USERNAME = "web_username"
CONF_WEB_PASSWORD = "web_password"

nightmatiq_ns = cg.esphome_ns.namespace("nightmatiq_mesh")
NightmatiqMesh = nightmatiq_ns.class_(
    "NightmatiqMesh", cg.PollingComponent, esp32_ble_tracker.ESPBTDeviceListener
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NightmatiqMesh),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Required(CONF_LUX_SENSOR_ID): cv.use_id(sensor.Sensor),
        cv.Required(CONF_RSSI_SENSOR_ID): cv.use_id(sensor.Sensor),
        cv.Required(CONF_THRESHOLD_NUMBER_ID): cv.use_id(number.Number),
        cv.Required(CONF_MODE_SELECT_ID): cv.use_id(select.Select),
        cv.Required(CONF_READY_BINARY_SENSOR_ID): cv.use_id(binary_sensor.BinarySensor),
        cv.Required(CONF_ACTUAL_OUTPUT_BINARY_SENSOR_ID): cv.use_id(binary_sensor.BinarySensor),
        cv.Required(CONF_STATUS_TEXT_SENSOR_ID): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_FIRMWARE_VERSION_TEXT_SENSOR_ID): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_HARDWARE_VERSION_SENSOR_ID): cv.use_id(sensor.Sensor),
        cv.Required(CONF_MANUFACTURER_TEXT_SENSOR_ID): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_COMPANY_ID_TEXT_SENSOR_ID): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_PRODUCT_ID_TEXT_SENSOR_ID): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_EXTENDED_DIAGNOSTICS, default=True): cv.boolean,
        cv.Required(CONF_WEB_USERNAME): cv.string_strict,
        cv.Required(CONF_WEB_PASSWORD): cv.string_strict,
    }
).extend(cv.polling_component_schema("30s")).extend(
    esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA
)


async def to_code(config):
    if config[CONF_EXTENDED_DIAGNOSTICS]:
        cg.add_define("USE_NIGHTMATIQ_EXTENDED_DIAGNOSTICS")

    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)

    lux = await cg.get_variable(config[CONF_LUX_SENSOR_ID])
    rssi = await cg.get_variable(config[CONF_RSSI_SENSOR_ID])
    threshold = await cg.get_variable(config[CONF_THRESHOLD_NUMBER_ID])
    mode = await cg.get_variable(config[CONF_MODE_SELECT_ID])
    ready = await cg.get_variable(config[CONF_READY_BINARY_SENSOR_ID])
    actual_output = await cg.get_variable(config[CONF_ACTUAL_OUTPUT_BINARY_SENSOR_ID])
    status = await cg.get_variable(config[CONF_STATUS_TEXT_SENSOR_ID])
    firmware_version = await cg.get_variable(config[CONF_FIRMWARE_VERSION_TEXT_SENSOR_ID])
    hardware_version = await cg.get_variable(config[CONF_HARDWARE_VERSION_SENSOR_ID])
    manufacturer = await cg.get_variable(config[CONF_MANUFACTURER_TEXT_SENSOR_ID])
    company_id = await cg.get_variable(config[CONF_COMPANY_ID_TEXT_SENSOR_ID])
    product_id = await cg.get_variable(config[CONF_PRODUCT_ID_TEXT_SENSOR_ID])
    cg.add(var.set_lux_sensor(lux))
    cg.add(var.set_rssi_sensor(rssi))
    cg.add(var.set_threshold_number(threshold))
    cg.add(var.set_mode_select(mode))
    cg.add(var.set_ready_binary_sensor(ready))
    cg.add(var.set_actual_output_binary_sensor(actual_output))
    cg.add(var.set_status_text_sensor(status))
    cg.add(var.set_firmware_version_text_sensor(firmware_version))
    cg.add(var.set_hardware_version_sensor(hardware_version))
    cg.add(var.set_manufacturer_text_sensor(manufacturer))
    cg.add(var.set_company_id_text_sensor(company_id))
    cg.add(var.set_product_id_text_sensor(product_id))
    cg.add(var.set_web_credentials(config[CONF_WEB_USERNAME], config[CONF_WEB_PASSWORD]))
