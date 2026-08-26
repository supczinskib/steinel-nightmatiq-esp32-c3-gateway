#include "nightmatiq_mesh.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <new>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble/ble.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"

// ESP-IDF exposes read/delete operations for provisioner nodes publicly, but
// its settings loader uses this internal restore entry point to rebuild the
// same public esp_ble_mesh_node_t layout. We use it only to reconstruct the
// already-provisioned NightmatIQ address range from our authenticated backup.
extern "C" {
struct bt_mesh_node;
int bt_mesh_provisioner_restore_node_info(struct bt_mesh_node *node);

}

namespace esphome {
namespace nightmatiq_mesh {

static const char *const TAG = "nightmatiq_mesh";
static constexpr uint16_t AMBIENT_LIGHT_LEVEL_PROPERTY = 0x004E;
static constexpr uint16_t LC_LIGHT_ON_THRESHOLD_PROPERTY = 0x002B;
static constexpr uint8_t MESSAGE_TTL = 7;
// Normal state reads are local, single-hop Mesh traffic. A four-second client
// timeout made a user command wait behind a missed background response. Keep
// routine Access requests short; Composition Data has its own longer timeout.
static constexpr uint32_t MESSAGE_TIMEOUT_MS = 1200;

static void reboot_after_confirming_firmware() {
  const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result != ESP_OK)
    ESP_LOGW(TAG, "Could not confirm the running firmware before restart: %s",
             esp_err_to_name(result));
  App.safe_reboot();
}
static constexpr uint32_t COMPOSITION_MESSAGE_TIMEOUT_MS = 4000;
static constexpr uint32_t FAST_CONTROL_STEP_MS = 120;
static constexpr uint32_t MODE_CONFIRMATION_GRACE_MS = 10000;
static constexpr uint32_t IDENTITY_SCAN_WINDOW_MS = 30000;
static constexpr uint32_t IDENTITY_SCAN_PREPARE_TIMEOUT_MS = 15000;
static constexpr uint32_t IDENTITY_SCAN_STOP_TIMEOUT_MS = 2000;
static constexpr uint32_t IDENTITY_SCAN_COMMAND_SETTLE_MS = 250;
// Match the Android scanner that receives the Steinel SCAN_RSP: active BLE
// scanning with a 100 ms interval and a full 100 ms window.
static constexpr uint32_t IDENTITY_SCAN_INTERVAL_UNITS = 160;
static constexpr uint8_t COMPOSITION_FAST_RETRY_LIMIT = 3;
static constexpr uint32_t COMPOSITION_FAST_RETRY_MS = 1500;
static constexpr uint32_t COMPOSITION_BACKGROUND_RETRY_MS = 60000;
static constexpr uint32_t COMPOSITION_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t COMPOSITION_REQUEST_WATCHDOG_MS = COMPOSITION_MESSAGE_TIMEOUT_MS + 1500;

// Composition Version ID identifies the device composition, not the semantic
// application release. Never translate it into a firmware version. Versions
// and hardware revisions are accepted only from Steinel manufacturer data
// emitted by the NightmatIQ itself.

NightmatiqMesh *NightmatiqMesh::instance_ = nullptr;

static uint8_t device_uuid[16]{};
static esp_ble_mesh_cfg_srv_t config_server{};
static esp_ble_mesh_client_t config_client{};
static esp_ble_mesh_client_t onoff_client{};
static esp_ble_mesh_client_t sensor_client{};
static esp_ble_mesh_client_t scene_client{};
static esp_ble_mesh_client_t light_lc_client{};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(nullptr, &onoff_client),
    ESP_BLE_MESH_MODEL_SENSOR_CLI(nullptr, &sensor_client),
    ESP_BLE_MESH_MODEL_SCENE_CLI(nullptr, &scene_client),
    ESP_BLE_MESH_MODEL_LIGHT_LC_CLI(nullptr, &light_lc_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition{};
static esp_ble_mesh_prov_t *provision = nullptr;

float NightmatiqMesh::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH - 1.0f; }

bool NightmatiqMesh::load_advertised_identity_() {
  StoredAdvertisedIdentity stored{};
  if (!this->advertised_identity_preference_.load(&stored) ||
      stored.magic != ADVERTISED_IDENTITY_MAGIC ||
      stored.version != ADVERTISED_IDENTITY_VERSION ||
      stored.product_id != NIGHTMATIQ_PRODUCT_ID)
    return false;

  this->advertised_product_id_.store(stored.product_id);
  this->advertised_firmware_major_.store(stored.firmware_major);
  this->advertised_firmware_minor_.store(stored.firmware_minor);
  this->advertised_firmware_patch_.store(stored.firmware_patch);
  this->advertised_bootloader_version_.store(stored.bootloader_version);
  this->advertised_hardware_version_.store(stored.hardware_version);
  this->advertised_firmware_hash_.store(stored.firmware_hash);
  this->advertised_composition_version_id_.store(stored.composition_version_id);
  this->advertised_identity_valid_.store(true);
  this->advertised_identity_publish_pending_.store(true);
  return true;
}

bool NightmatiqMesh::save_advertised_identity_(const StoredAdvertisedIdentity &identity) {
  StoredAdvertisedIdentity saved{};
  const bool unchanged = this->advertised_identity_preference_.load(&saved) &&
                         saved.magic == ADVERTISED_IDENTITY_MAGIC &&
                         saved.version == ADVERTISED_IDENTITY_VERSION &&
                         saved.product_id == identity.product_id &&
                         saved.firmware_major == identity.firmware_major &&
                         saved.firmware_minor == identity.firmware_minor &&
                         saved.firmware_patch == identity.firmware_patch &&
                         saved.bootloader_version == identity.bootloader_version &&
                         saved.hardware_version == identity.hardware_version &&
                         saved.firmware_hash == identity.firmware_hash &&
                         saved.composition_version_id == identity.composition_version_id;
  if (!unchanged && !this->advertised_identity_preference_.save(&identity))
    return false;

  this->advertised_product_id_.store(identity.product_id);
  this->advertised_firmware_major_.store(identity.firmware_major);
  this->advertised_firmware_minor_.store(identity.firmware_minor);
  this->advertised_firmware_patch_.store(identity.firmware_patch);
  this->advertised_bootloader_version_.store(identity.bootloader_version);
  this->advertised_hardware_version_.store(identity.hardware_version);
  this->advertised_firmware_hash_.store(identity.firmware_hash);
  this->advertised_composition_version_id_.store(identity.composition_version_id);
  this->advertised_identity_valid_.store(true);
  this->advertised_identity_publish_pending_.store(true);
  return true;
}

void NightmatiqMesh::clear_advertised_identity_() {
  StoredAdvertisedIdentity empty{};
  empty.magic = 0;
  this->advertised_identity_preference_.save(&empty);
  this->advertised_product_id_.store(0);
  this->advertised_firmware_major_.store(0);
  this->advertised_firmware_minor_.store(0);
  this->advertised_firmware_patch_.store(0);
  this->advertised_bootloader_version_.store(0);
  this->advertised_hardware_version_.store(0);
  this->advertised_firmware_hash_.store(0);
  this->advertised_composition_version_id_.store(0);
  this->advertised_rssi_.store(0);
  this->advertised_identity_valid_.store(false);
  this->advertised_identity_fresh_.store(false);
  this->advertised_identity_publish_pending_.store(true);
}

bool NightmatiqMesh::advertised_identity_current_() const {
  if (!this->advertised_identity_valid_.load() || !this->composition_received_.load())
    return false;
  if (this->advertised_product_id_.load() != this->live_product_id_.load())
    return false;
  if (this->identity_found_this_boot_.load())
    return true;
  const uint16_t associated_vid = this->advertised_composition_version_id_.load();
  return associated_vid != 0 && associated_vid == this->live_version_id_.load();
}

bool NightmatiqMesh::resolve_firmware_version_(uint8_t &major, uint8_t &minor,
                                               uint8_t &patch) const {
  if (this->advertised_identity_current_()) {
    major = this->advertised_firmware_major_.load();
    minor = this->advertised_firmware_minor_.load();
    patch = this->advertised_firmware_patch_.load();
    return true;
  }
  return false;
}

bool NightmatiqMesh::parse_scan_result_(const esp32_ble::BLEScanResult &result) {
  if (!this->identity_scan_pending_.load())
    return false;

  const size_t total_length = static_cast<size_t>(result.adv_data_len) + result.scan_rsp_len;
  size_t offset = 0;
  while (offset < total_length) {
    const uint8_t field_length = result.ble_adv[offset++];
    if (field_length == 0)
      continue;
    if (offset + field_length > total_length)
      break;

    const uint8_t field_type = result.ble_adv[offset++];
    const size_t value_length = static_cast<size_t>(field_length) - 1;
    const uint8_t *value = result.ble_adv + offset;
    offset += value_length;
    if (field_type != ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE || value_length < 2)
      continue;

    const uint16_t company_id = static_cast<uint16_t>(value[0]) |
                                (static_cast<uint16_t>(value[1]) << 8);
    if (company_id != STEINEL_COMPANY_ID)
      continue;
    if (this->capture_advertised_identity_(value + 2, value_length - 2, result.rssi))
      return true;
  }
  return false;
}

void NightmatiqMesh::gap_scan_event_handler(const esp32_ble::BLEScanResult &scan_result) {
  if (scan_result.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
    this->identity_scan_stop_ready_.store(true);
    return;
  }
  this->parse_scan_result_(scan_result);
}

void NightmatiqMesh::gap_event_handler(esp_gap_ble_cb_event_t event,
                                       esp_ble_gap_cb_param_t *param) {
  if (!this->identity_scan_pending_.load() || param == nullptr)
    return;
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
        this->identity_scan_params_ready_.store(true);
      else
        this->identity_scan_command_error_.store(true);
      break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
        this->identity_scan_start_ready_.store(true);
      else
        this->identity_scan_command_error_.store(true);
      break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        this->identity_scan_command_error_.store(true);
      this->identity_scan_stop_ready_.store(true);
      break;
    default:
      break;
  }
}

bool NightmatiqMesh::capture_advertised_identity_(const uint8_t *data, size_t length, int16_t rssi) {
  if (data == nullptr || length < 7)
    return false;
  const uint16_t product_id = static_cast<uint16_t>(data[0]) |
                              (static_cast<uint16_t>(data[1]) << 8);
  if (product_id != NIGHTMATIQ_PRODUCT_ID)
    return false;

  this->advertised_product_id_.store(product_id);
  this->advertised_firmware_major_.store(data[2]);
  this->advertised_firmware_minor_.store(data[3]);
  this->advertised_firmware_patch_.store(data[4]);
  this->advertised_bootloader_version_.store(data[5]);
  this->advertised_hardware_version_.store(data[6]);
  this->advertised_firmware_hash_.store(
      length >= 9 ? static_cast<uint16_t>(data[7]) |
                        (static_cast<uint16_t>(data[8]) << 8)
                  : 0);
  this->advertised_composition_version_id_.store(
      this->composition_received_.load() ? this->live_version_id_.load() : 0);
  this->advertised_rssi_.store(rssi);
  this->advertised_identity_valid_.store(true);
  this->advertised_identity_fresh_.store(true);
  this->advertised_identity_publish_pending_.store(true);
  this->advertised_identity_save_pending_.store(true);
  this->identity_found_this_boot_.store(true);
  ESP_LOGI(TAG, "NightmatIQ advertisement: product=0x%04X firmware=%u.%u.%u bootloader=%u hardware=%u RSSI=%d dBm",
           product_id, data[2], data[3], data[4], data[5], data[6], rssi);
  return true;
}

void NightmatiqMesh::begin_identity_scan_() {
  this->identity_found_this_boot_.store(false);
  this->advertised_identity_fresh_.store(false);
  this->identity_scan_started_ = false;
  this->identity_scan_phase_ = IdentityScanPhase::WAIT_FOR_BLE;
  this->identity_scan_action_at_ = 0;
  this->identity_scan_params_ready_.store(false);
  this->identity_scan_start_ready_.store(false);
  this->identity_scan_stop_ready_.store(false);
  this->identity_scan_command_error_.store(false);
  this->identity_scan_params_ = {};
  this->identity_scan_params_.scan_type = BLE_SCAN_TYPE_ACTIVE;
  this->identity_scan_params_.own_addr_type = BLE_ADDR_TYPE_RANDOM;
  this->identity_scan_params_.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
  this->identity_scan_params_.scan_interval = IDENTITY_SCAN_INTERVAL_UNITS;
  this->identity_scan_params_.scan_window = IDENTITY_SCAN_INTERVAL_UNITS;
  this->identity_scan_params_.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
  this->identity_scan_pending_.store(true);
  this->identity_scan_deadline_ = millis() + IDENTITY_SCAN_PREPARE_TIMEOUT_MS;
  this->set_status_("Reading NightmatIQ device report");
}

void NightmatiqMesh::advance_identity_scan_() {
  if (!this->identity_scan_pending_.load())
    return;
  const uint32_t now = millis();
  const bool expired = static_cast<int32_t>(now - this->identity_scan_deadline_) >= 0;

  if (this->identity_scan_phase_ == IdentityScanPhase::WAIT_FOR_SCAN_STOP) {
    if (!this->identity_scan_stop_ready_.exchange(false) && !expired)
      return;
    this->finish_identity_scan_();
    return;
  }

  if (this->identity_scan_command_error_.load() || expired) {
    if (this->identity_scan_started_) {
      const esp_err_t error = esp_ble_gap_stop_scanning();
      if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
        ESP_LOGE(TAG, "Could not stop NightmatIQ identity scan: %s", esp_err_to_name(error));
      if (error == ESP_ERR_INVALID_STATE)
        this->identity_scan_stop_ready_.store(true);
      this->identity_scan_command_error_.store(false);
      this->identity_scan_phase_ = IdentityScanPhase::WAIT_FOR_SCAN_STOP;
      this->identity_scan_deadline_ = now + IDENTITY_SCAN_STOP_TIMEOUT_MS;
      return;
    }
    this->finish_identity_scan_();
    return;
  }

  switch (this->identity_scan_phase_) {
    case IdentityScanPhase::WAIT_FOR_BLE: {
      if (esp32_ble::global_ble == nullptr || !esp32_ble::global_ble->is_active() ||
          esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
        return;
      esp_bd_addr_t random_address{};
      esp_ble_gap_addr_create_static(random_address);
      const esp_err_t error = esp_ble_gap_set_rand_addr(random_address);
      if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not set random BLE scanner address: %s", esp_err_to_name(error));
        this->identity_scan_command_error_.store(true);
        return;
      }
      this->identity_scan_phase_ = IdentityScanPhase::WAIT_FOR_RANDOM_ADDRESS;
      this->identity_scan_action_at_ = now + IDENTITY_SCAN_COMMAND_SETTLE_MS;
      return;
    }
    case IdentityScanPhase::WAIT_FOR_RANDOM_ADDRESS:
      if (static_cast<int32_t>(now - this->identity_scan_action_at_) < 0)
        return;
      if (const esp_err_t error = esp_ble_gap_set_scan_params(&this->identity_scan_params_);
          error != ESP_OK) {
        ESP_LOGE(TAG, "Could not set NightmatIQ scan parameters: %s", esp_err_to_name(error));
        this->identity_scan_command_error_.store(true);
        return;
      }
      this->identity_scan_phase_ = IdentityScanPhase::WAIT_FOR_SCAN_PARAMS;
      return;
    case IdentityScanPhase::WAIT_FOR_SCAN_PARAMS:
      if (!this->identity_scan_params_ready_.exchange(false))
        return;
      if (const esp_err_t error = esp_ble_gap_start_scanning(IDENTITY_SCAN_WINDOW_MS / 1000U);
          error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start NightmatIQ identity scan: %s", esp_err_to_name(error));
        this->identity_scan_command_error_.store(true);
        return;
      }
      this->identity_scan_phase_ = IdentityScanPhase::WAIT_FOR_SCAN_START;
      return;
    case IdentityScanPhase::WAIT_FOR_SCAN_START:
      if (!this->identity_scan_start_ready_.exchange(false))
        return;
      this->identity_scan_started_ = true;
      this->identity_scan_phase_ = IdentityScanPhase::RUNNING;
      this->identity_scan_deadline_ = now + IDENTITY_SCAN_WINDOW_MS;
      ESP_LOGI(TAG, "Active NightmatIQ identity scan started with a random scanner address");
      return;
    case IdentityScanPhase::RUNNING:
      if (!this->identity_found_this_boot_.load() && !this->identity_scan_stop_ready_.load())
        return;
      if (!this->identity_scan_stop_ready_.load()) {
        const esp_err_t error = esp_ble_gap_stop_scanning();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
          ESP_LOGE(TAG, "Could not stop NightmatIQ identity scan: %s", esp_err_to_name(error));
          this->identity_scan_command_error_.store(true);
          return;
        }
        if (error == ESP_ERR_INVALID_STATE)
          this->identity_scan_stop_ready_.store(true);
      }
      this->identity_scan_phase_ = IdentityScanPhase::WAIT_FOR_SCAN_STOP;
      this->identity_scan_deadline_ = now + IDENTITY_SCAN_STOP_TIMEOUT_MS;
      return;
    case IdentityScanPhase::WAIT_FOR_SCAN_STOP:
      return;
  }
}

void NightmatiqMesh::finish_identity_scan_() {
  if (!this->identity_found_this_boot_.load())
    ESP_LOGW(TAG, "NightmatIQ device report was not received during the active scan window");
  this->identity_scan_pending_.store(false);
  this->identity_scan_started_ = false;
  this->mesh_start_pending_ = true;
  this->mesh_start_not_before_ = millis() + 250;
  this->mesh_start_deadline_ = millis() + 7500;
  this->set_status_("Preparing Bluetooth Mesh");
}

void NightmatiqMesh::persist_pending_advertised_identity_() {
  if (!this->advertised_identity_save_pending_.exchange(false))
    return;
  StoredAdvertisedIdentity identity{};
  identity.product_id = this->advertised_product_id_.load();
  identity.firmware_major = this->advertised_firmware_major_.load();
  identity.firmware_minor = this->advertised_firmware_minor_.load();
  identity.firmware_patch = this->advertised_firmware_patch_.load();
  identity.bootloader_version = this->advertised_bootloader_version_.load();
  identity.hardware_version = this->advertised_hardware_version_.load();
  identity.firmware_hash = this->advertised_firmware_hash_.load();
  identity.composition_version_id = this->advertised_composition_version_id_.load();
  if (!this->save_advertised_identity_(identity))
    ESP_LOGW(TAG, "Could not persist advertised NightmatIQ identity");
}

bool NightmatiqMesh::initialize_bluetooth_() {
  esp_err_t error;
  esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();

  if (controller_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
    // Classic Bluetooth is unused. Ignore "already released" on installations
    // where ESPHome initialized the shared BLE host before this component.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    error = esp_bt_controller_init(&controller_config);
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Bluetooth controller initialization failed: %s", esp_err_to_name(error));
      this->set_status_(std::string("Bluetooth controller initialization failed: ") + esp_err_to_name(error));
      return false;
    }
    controller_status = esp_bt_controller_get_status();
  }

  if (controller_status == ESP_BT_CONTROLLER_STATUS_INITED) {
    error = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(error));
      this->set_status_(std::string("Bluetooth controller enable failed: ") + esp_err_to_name(error));
      return false;
    }
  }

  esp_bluedroid_status_t host_status = esp_bluedroid_get_status();
  if (host_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    error = esp_bluedroid_init();
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Bluedroid initialization failed: %s", esp_err_to_name(error));
      this->set_status_(std::string("Bluedroid initialization failed: ") + esp_err_to_name(error));
      return false;
    }
    host_status = esp_bluedroid_get_status();
  }
  if (host_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
    error = esp_bluedroid_enable();
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(error));
      this->set_status_(std::string("Bluedroid enable failed: ") + esp_err_to_name(error));
      return false;
    }
  }

  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
         esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
}

bool NightmatiqMesh::initialize_mesh_() {
  const uint8_t *address = esp_bt_dev_get_address();
  if (address == nullptr) {
    ESP_LOGE(TAG, "Bluetooth device address is unavailable");
    this->set_status_("Bluetooth device address is unavailable");
    return false;
  }
  device_uuid[0] = 0x4E;  // "NM", only a local provisioner identity marker.
  device_uuid[1] = 0x4D;
  std::memcpy(device_uuid + 2, address, 6);

  config_server.net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20);
  config_server.relay = ESP_BLE_MESH_RELAY_DISABLED;
  config_server.relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20);
  config_server.beacon = ESP_BLE_MESH_BEACON_ENABLED;
  config_server.gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED;
  config_server.friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED;
  config_server.default_ttl = MESSAGE_TTL;

  composition.cid = 0x02E5;  // Espressif company identifier used by their examples.
  composition.element_count = sizeof(elements) / sizeof(elements[0]);
  composition.elements = elements;

  // prov_unicast_addr is const in ESP-IDF. Build the provisioning context only
  // after the saved network has been loaded, so the runtime-selected free
  // address is initialized legally instead of assigned after construction.
  if (provision != nullptr) {
    ESP_LOGE(TAG, "Bluetooth Mesh provisioning context is already initialized");
    this->set_status_("Bluetooth Mesh provisioning context is already initialized");
    return false;
  }
  provision = new (std::nothrow) esp_ble_mesh_prov_t{
      .prov_uuid = device_uuid,
      .prov_unicast_addr = this->config_.local_address,
      .prov_start_address = static_cast<uint16_t>(this->config_.local_address + 1),
      .prov_attention = 0,
      .prov_algorithm = 0,
      .prov_pub_key_oob = 0,
      .prov_static_oob_val = nullptr,
      .prov_static_oob_len = 0,
      .flags = 0,
      .iv_index = this->config_.iv_index,
  };
  if (provision == nullptr) {
    ESP_LOGE(TAG, "Could not allocate Bluetooth Mesh provisioning context");
    this->set_status_("Could not allocate Bluetooth Mesh provisioning context");
    return false;
  }

  esp_ble_mesh_register_prov_callback(NightmatiqMesh::provisioning_callback);
  esp_ble_mesh_register_config_client_callback(NightmatiqMesh::config_callback);
  esp_ble_mesh_register_generic_client_callback(NightmatiqMesh::generic_callback);
  esp_ble_mesh_register_sensor_client_callback(NightmatiqMesh::sensor_callback);
  esp_ble_mesh_register_light_client_callback(NightmatiqMesh::light_callback);
  esp_ble_mesh_register_time_scene_client_callback(NightmatiqMesh::scene_callback);

  this->set_status_("Initializing ESP-BLE-MESH core");
  esp_err_t error = esp_ble_mesh_init(provision, &composition);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "ESP-BLE-MESH initialization failed: %s", esp_err_to_name(error));
    this->set_status_(std::string("ESP-BLE-MESH initialization failed: ") + esp_err_to_name(error));
    delete provision;
    provision = nullptr;
    return false;
  }
  // From this point the Mesh core must be deinitialized explicitly even if a
  // later bearer or key-import step fails.
  this->mesh_started_ = true;
  // ESP-IDF restores persisted model bindings after initializing the Config
  // Client and can overwrite its mandatory DeviceKey binding with an unused
  // value. Configuration messages are DeviceKey-only; restore the SIG-defined
  // binding explicitly after the settings load has completed.
  if (config_client.model != nullptr)
    config_client.model->keys[0] = ESP_BLE_MESH_KEY_DEV;
  this->live_iv_index_.store(this->config_.iv_index);
  this->live_iv_index_confirmed_.store(false);
  this->iv_index_check_at_ = millis() + 1000;

  // ESP-IDF persists the provisioner's primary address independently of the
  // application configuration.  A value restored from Mesh NVS takes
  // precedence over prov_unicast_addr, so explicitly synchronize it before
  // enabling the provisioner bearer.  The bearer is enabled from the
  // completion callback to preserve the required asynchronous ordering.
  this->set_status_("Synchronizing Bluetooth Mesh provisioner address");
  error = esp_ble_mesh_provisioner_set_primary_elem_addr(this->config_.local_address);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "Mesh provisioner address request failed: %s", esp_err_to_name(error));
    this->set_status_(std::string("Mesh provisioner address request failed: ") + esp_err_to_name(error));
    return false;
  }
  return true;
}

bool NightmatiqMesh::deinitialize_mesh_(bool erase_flash) {
  if (!this->mesh_started_)
    return true;

  this->mesh_ready_.store(false);
  this->ready_publish_pending_.store(true);
  this->keys_bound_pending_ = false;
  this->poll_stage_ = 0;
  this->access_operation_.store(AccessOperation::NONE);
  this->access_opcode_.store(0);
  this->control_kind_ = ControlKind::NONE;
  this->control_step_ = ControlStep::IDLE;
  this->mode_override_pending_.store(false);
  this->threshold_override_pending_.store(false);
  this->composition_query_pending_.store(false);
  this->composition_query_in_flight_.store(false);

  // ESP-IDF requires every client model to be deinitialized before the Mesh
  // core. The configuration server at index 0 is owned by the core itself.
  esp_err_t first_error = ESP_OK;
  for (size_t index = 1; index < sizeof(root_models) / sizeof(root_models[0]); index++) {
    const esp_err_t error = esp_ble_mesh_client_model_deinit(&root_models[index]);
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Mesh client model %u deinit failed: %s",
               static_cast<unsigned>(index), esp_err_to_name(error));
      if (first_error == ESP_OK)
        first_error = error;
    }
  }

  esp_ble_mesh_deinit_param_t parameters{.erase_flash = erase_flash};
  const esp_err_t deinit_error = esp_ble_mesh_deinit(&parameters);
  if (deinit_error != ESP_OK) {
    ESP_LOGE(TAG, "ESP-BLE-MESH deinit failed: %s", esp_err_to_name(deinit_error));
    this->set_status_(std::string("Bluetooth Mesh cleanup failed: ") + esp_err_to_name(deinit_error));
    return false;
  }

  delete provision;
  provision = nullptr;
  this->mesh_started_ = false;
  this->mesh_start_pending_ = false;
  this->live_iv_index_confirmed_.store(false);
  if (first_error != ESP_OK)
    ESP_LOGW(TAG, "Mesh core cleanup completed after a client model deinit error");
  return true;
}

bool NightmatiqMesh::restore_target_node_() {
  if (esp_ble_mesh_provisioner_get_node_with_addr(this->config_.onoff_address) != nullptr)
    return true;

  esp_ble_mesh_node_t node{};
  node.unicast_addr = this->config_.onoff_address;
  // NightmatIQ Plus has three elements in the authenticated Steinel backup.
  // The provisioner checks this range before it permits any unicast Access
  // message or accepts a response from the device.
  node.element_num = 3;
  node.net_idx = this->config_.net_key_index;
  node.flags = 0;
  node.iv_index = this->config_.iv_index;
  if (this->device_key_valid_)
    std::memcpy(node.dev_key, this->device_key_.data(), sizeof(node.dev_key));
  std::memcpy(node.dev_uuid, this->config_.mesh_uuid.data(), sizeof(node.dev_uuid));
  node.dev_uuid[14] ^= static_cast<uint8_t>(this->config_.onoff_address >> 8);
  node.dev_uuid[15] ^= static_cast<uint8_t>(this->config_.onoff_address & 0xFF);
  std::strncpy(node.name, this->config_.node_name, sizeof(node.name) - 1);

  const int error = bt_mesh_provisioner_restore_node_info(reinterpret_cast<bt_mesh_node *>(&node));
  if (error != 0) {
    ESP_LOGE(TAG, "Could not restore NightmatIQ node 0x%04X: %d", this->config_.onoff_address, error);
    this->set_status_("Could not restore NightmatIQ node: " + std::to_string(error));
    return false;
  }
  ESP_LOGI(TAG, "Restored NightmatIQ node address range 0x%04X-0x%04X",
           this->config_.onoff_address, this->config_.onoff_address + node.element_num - 1);
  return true;
}

void NightmatiqMesh::advance_mesh_start_() {
  if (!this->mesh_start_pending_)
    return;

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->mesh_start_not_before_) < 0)
    return;

  const bool scan_idle = !this->identity_scan_pending_.load() && !this->identity_scan_started_;
  if (!scan_idle) {
    if (static_cast<int32_t>(now - this->mesh_start_deadline_) < 0)
      return;
    this->mesh_start_pending_ = false;
    ESP_LOGE(TAG, "Bluetooth scan did not stop before Mesh startup");
    this->set_status_("Could not stop Bluetooth scan before Mesh startup");
    return;
  }

  this->mesh_start_pending_ = false;
  ESP_LOGI(TAG, "Starting Mesh after Bluetooth scan stopped: free heap=%u, largest block=%u",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  this->set_status_("Starting Bluetooth Mesh");
  if (!this->initialize_bluetooth_() || !this->initialize_mesh_())
    return;

  if ((this->config_.flags & FLAG_REMOVE_PENDING) != 0) {
    this->set_status_("Removing Bluetooth Mesh configuration");
    this->mesh_remove_pending_.store(true);
  }
}

void NightmatiqMesh::advance_mesh_remove_() {
  if (!this->mesh_remove_pending_.load())
    return;
  if (static_cast<int32_t>(millis() - this->mesh_remove_not_before_) < 0)
    return;
  if (!this->mesh_remove_pending_.exchange(false))
    return;

  this->set_status_("Stopping Bluetooth Mesh");
  if (!this->deinitialize_mesh_(true))
    return;

  this->clear_config_();
  this->cloud_ble_resume_pending_.store(true);
  this->set_status_("Configuration removed; gateway ready for setup");
}

void NightmatiqMesh::advance_factory_reset_() {
  if (!this->factory_reset_pending_.load() ||
      static_cast<int32_t>(millis() - this->factory_reset_at_) < 0)
    return;
  if (!this->factory_reset_pending_.exchange(false))
    return;

  ESP_LOGW(TAG, "Erasing all gateway settings and restoring factory defaults");
  if (!global_preferences->reset()) {
    this->set_status_("Factory reset failed");
    return;
  }
  delay(100);
  reboot_after_confirming_firmware();
}

void NightmatiqMesh::monitor_iv_index_() {
  if (!this->mesh_started_)
    return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->iv_index_check_at_) < 0)
    return;
  this->iv_index_check_at_ = now + 1000;

  const uint32_t live_iv_index = bt_mesh.iv_index;
  this->live_iv_index_.store(live_iv_index);
  // A changed value has passed Secure Network Beacon authentication inside
  // ESP-IDF. A successfully decoded Access response independently proves that
  // the current value is usable even when it equals the imported starting
  // value.
  const bool authenticated = live_iv_index != this->config_.iv_index ||
                             this->mesh_rx_messages_.load() > 0;
  if (!authenticated)
    return;
  this->live_iv_index_confirmed_.store(true);
  if (live_iv_index == this->config_.iv_index &&
      (this->config_.flags & FLAG_IV_INDEX_CONFIRMED) != 0)
    return;

  StoredConfig updated = this->config_;
  const uint32_t previous_iv_index = updated.iv_index;
  updated.iv_index = live_iv_index;
  updated.flags |= FLAG_IV_INDEX_CONFIRMED;
  if (this->save_config_(updated)) {
    ESP_LOGI(TAG, "Persisted authenticated IV Index: %" PRIu32 " -> %" PRIu32 "%s",
             previous_iv_index, live_iv_index,
             previous_iv_index == live_iv_index ? " (confirmed)" : "");
  } else {
    ESP_LOGW(TAG, "Could not persist authenticated IV Index %" PRIu32, live_iv_index);
  }
}

bool NightmatiqMesh::valid_admin_password_(const std::string &password) {
  if (password.size() < ADMIN_PASSWORD_MIN_LENGTH ||
      password.size() > ADMIN_PASSWORD_MAX_LENGTH)
    return false;
  return std::all_of(password.begin(), password.end(), [](unsigned char value) {
    return value >= 0x21 && value <= 0x7E;
  });
}

bool NightmatiqMesh::load_admin_credentials_() {
  StoredAdminCredentials stored{};
  if (!this->admin_credentials_preference_.load(&stored) ||
      stored.magic != ADMIN_CREDENTIALS_MAGIC ||
      stored.version != ADMIN_CREDENTIALS_VERSION ||
      stored.password[ADMIN_PASSWORD_MAX_LENGTH] != '\0') {
    this->using_factory_admin_password_ = true;
    return false;
  }

  const std::string password(stored.password);
  if (!valid_admin_password_(password)) {
    this->using_factory_admin_password_ = true;
    return false;
  }

  this->web_password_ = password;
  this->using_factory_admin_password_ = false;
  return true;
}

bool NightmatiqMesh::save_admin_password_(const std::string &password) {
  if (!valid_admin_password_(password))
    return false;

  StoredAdminCredentials stored{};
  std::memcpy(stored.password, password.data(), password.size());
  stored.password[password.size()] = '\0';
  if (!this->admin_credentials_preference_.save(&stored))
    return false;

  std::fill(this->web_password_.begin(), this->web_password_.end(), '\0');
  this->web_password_ = password;
  this->using_factory_admin_password_ = false;
  this->apply_admin_credentials_();
  return true;
}

void NightmatiqMesh::apply_admin_credentials_() {
  this->base_->set_auth_username(this->web_username_);
  this->base_->set_auth_password(this->web_password_);
  if (this->ota_ != nullptr)
    this->ota_->set_auth_password(this->web_password_);
}

void NightmatiqMesh::setup() {
  ESP_LOGCONFIG(TAG, "Setting up NightmatIQ cloud and Bluetooth Mesh client");
  this->instance_ = this;
  this->config_preference_ = global_preferences->make_preference<StoredConfig>(0x4E4D5101U);
  this->device_key_preference_ = global_preferences->make_preference<StoredDeviceKey>(0x4E4D5102U);
  this->retired_address_preference_ =
      global_preferences->make_preference<StoredRetiredAddress>(0x4E4D5103U);
  this->iv_cache_preference_ = global_preferences->make_preference<StoredIvCache>(0x4E4D5104U);
  this->advertised_identity_preference_ =
      global_preferences->make_preference<StoredAdvertisedIdentity>(0x4E4D5105U);
  this->address_policy_preference_ =
      global_preferences->make_preference<StoredAddressPolicy>(0x4E4D5106U);
  this->address_confirmation_preference_ =
      global_preferences->make_preference<StoredAddressConfirmation>(0x4E4D5107U);
  this->admin_credentials_preference_ =
      global_preferences->make_preference<StoredAdminCredentials>(0x4E4D5108U);
  this->auto_update_preference_ =
      global_preferences->make_preference<StoredAutoUpdate>(0x4E4D5109U);
  this->load_admin_credentials_();
  this->apply_admin_credentials_();
  this->base_->add_handler(this);
  const bool auto_update_pending = this->load_auto_update_();
  this->load_advertised_identity_();
  this->load_retired_address_();
  this->load_address_policy_();
  this->load_address_confirmation_();
  if (this->ready_binary_sensor_ != nullptr)
    this->ready_binary_sensor_->publish_state(false);
  const bool has_config = this->load_config_();
  if (has_config) {
    this->load_device_key_();
    this->mesh_mode_enabled_ =
        (this->config_.flags & (FLAG_ENABLED | FLAG_REMOVE_PENDING)) != 0;
  }
  if (auto_update_pending) {
    this->set_status_("Firmware update pending; waiting for network");
    return;
  }
  if (!has_config) {
    this->set_status_("Gateway ready; configure NightmatIQ on this page");
    return;
  }
  if (!this->mesh_mode_enabled_) {
    this->set_status_("NightmatIQ disabled; gateway in setup mode");
    return;
  }
  this->actual_output_forced_unavailable_.store(false);
  this->begin_identity_scan_();
}

void NightmatiqMesh::dump_config() {
  ESP_LOGCONFIG(TAG, "NightmatIQ Bluetooth Mesh:");
  ESP_LOGCONFIG(TAG, "  Configuration: %s", YESNO(this->configured_));
  if (!this->configured_)
    return;
  ESP_LOGCONFIG(TAG, "  Integration enabled: %s", YESNO(this->mesh_mode_enabled_));
  ESP_LOGCONFIG(TAG, "  BLE runtime mode: %s", this->mesh_mode_enabled_ ? "Bluetooth Mesh" : "Setup");
  ESP_LOGCONFIG(TAG, "  Network: %s", this->config_.network_name);
  ESP_LOGCONFIG(TAG, "  Node: %s", this->config_.node_name);
  ESP_LOGCONFIG(TAG, "  Initial IV Index: %" PRIu32, this->config_.iv_index);
  ESP_LOGCONFIG(TAG, "  Local address: 0x%04X", this->config_.local_address);
  ESP_LOGCONFIG(TAG, "  NightmatIQ elements: OnOff=0x%04X LC=0x%04X Sensor=0x%04X",
                this->config_.onoff_address, this->config_.lc_address, this->config_.sensor_address);
  ESP_LOGCONFIG(TAG, "  Nightmatic scene: %u", this->config_.scene_number);
  ESP_LOGCONFIG(TAG, "  DeviceKey available for authenticated identity: %s", YESNO(this->device_key_valid_));
  if (this->advertised_identity_valid_.load()) {
    ESP_LOGCONFIG(TAG, "  Device-reported firmware: %u.%u.%u", this->advertised_firmware_major_.load(),
                  this->advertised_firmware_minor_.load(), this->advertised_firmware_patch_.load());
    ESP_LOGCONFIG(TAG, "  Device-reported bootloader/hardware: %u/%u",
                  this->advertised_bootloader_version_.load(), this->advertised_hardware_version_.load());
  }
  ESP_LOGCONFIG(TAG, "  Mesh models ready: %s", YESNO(this->mesh_ready_.load()));
}

bool NightmatiqMesh::set_common_(esp_ble_mesh_client_common_param_t &common, esp_ble_mesh_model_t *model,
                                 uint32_t opcode, uint16_t destination) {
  if (!this->mesh_ready_.load() || model == nullptr)
    return false;
  std::memset(&common, 0, sizeof(common));
  common.opcode = opcode;
  common.model = model;
  common.ctx.net_idx = this->config_.net_key_index;
  common.ctx.app_idx = this->config_.app_key_index;
  common.ctx.addr = destination;
  common.ctx.send_ttl = MESSAGE_TTL;
  common.msg_timeout = MESSAGE_TIMEOUT_MS;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
  common.msg_role = ROLE_PROVISIONER;
#endif
  return true;
}

bool NightmatiqMesh::record_send_result_(esp_err_t result) {
  this->mesh_tx_attempts_.fetch_add(1);
  this->mesh_last_tx_error_.store(result);
  if (result == ESP_OK) {
    this->mesh_tx_accepted_.fetch_add(1);
    return true;
  }
  this->mesh_tx_errors_.fetch_add(1);
  return false;
}

bool NightmatiqMesh::begin_access_operation_(AccessOperation operation, uint32_t opcode) {
  AccessOperation expected = AccessOperation::NONE;
  if (!this->access_operation_.compare_exchange_strong(expected, operation)) {
    ESP_LOGW(TAG, "Access request 0x%08" PRIX32 " deferred; another acknowledged request is active",
             opcode);
    return false;
  }
  this->access_opcode_.store(opcode);
  this->access_deadline_.store(millis() + MESSAGE_TIMEOUT_MS + 750);
  return true;
}

bool NightmatiqMesh::record_access_send_result_(AccessOperation operation, uint32_t opcode,
                                                esp_err_t result) {
  const bool accepted = this->record_send_result_(result);
  if (!accepted)
    this->complete_access_operation_(opcode, false);
  return accepted;
}

bool NightmatiqMesh::complete_access_operation_(uint32_t opcode, bool success) {
  if (this->access_opcode_.load() != opcode)
    return false;
  const AccessOperation operation = this->access_operation_.load();
  if (operation == AccessOperation::NONE)
    return false;

  // Publish the completion before releasing the global access slot. The main
  // loop can then safely advance a control transaction as soon as it observes
  // NONE, without racing a stale result from the callback task.
  this->access_last_completed_.store(operation);
  this->access_last_success_.store(success);
  this->access_opcode_.store(0);
  this->access_operation_.store(AccessOperation::NONE);
  return true;
}

void NightmatiqMesh::expire_access_operation_(uint32_t now) {
  const AccessOperation operation = this->access_operation_.load();
  if (operation == AccessOperation::NONE ||
      static_cast<int32_t>(now - this->access_deadline_.load()) < 0)
    return;
  const uint32_t opcode = this->access_opcode_.load();
  if (this->complete_access_operation_(opcode, false)) {
    this->mesh_timeouts_.fetch_add(1);
    ESP_LOGW(TAG, "Access request watchdog expired for opcode 0x%08" PRIX32, opcode);
  }
}

void NightmatiqMesh::bind_model_(uint16_t model_id) {
  const uint16_t local_address = esp_ble_mesh_get_primary_element_address();
  if (local_address == ESP_BLE_MESH_ADDR_UNASSIGNED) {
    ESP_LOGE(TAG, "Cannot bind AppKey to model 0x%04X: local primary element is unassigned", model_id);
    return;
  }
  if (local_address != this->config_.local_address) {
    ESP_LOGW(TAG, "Local primary element is 0x%04X, expected 0x%04X", local_address,
             this->config_.local_address);
  }
  const esp_err_t error = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
      local_address, this->config_.app_key_index, model_id, ESP_BLE_MESH_CID_NVAL);
  if (error != ESP_OK)
    ESP_LOGE(TAG, "Local AppKey bind request for model 0x%04X failed: %s", model_id, esp_err_to_name(error));
}

void NightmatiqMesh::mark_ready_() {
  this->mesh_ready_.store(true);
  this->mesh_ready_at_ = millis();
  this->address_recovery_attempted_this_boot_ = false;
  this->ready_publish_pending_.store(true);
  this->set_status_("Mesh client ready; polling NightmatIQ");
  ESP_LOGI(TAG, "NightmatIQ mesh keys imported and all client models bound");
  if (this->device_key_valid_) {
    this->composition_query_attempts_.store(0);
    this->composition_query_failures_.store(0);
    this->composition_query_at_.store(millis() + 500);
    this->composition_query_pending_.store(true);
  } else {
    ESP_LOGW(TAG, "NightmatIQ DeviceKey is not stored; live product/version read is unavailable until reimport");
  }
}

void NightmatiqMesh::advance_address_recovery_(uint32_t now) {
  if (this->address_recovery_attempted_this_boot_ || !this->mesh_ready_.load() ||
      !this->configured_ || !this->mesh_mode_enabled_ ||
      !this->address_policy_valid_ || this->current_address_confirmed_() ||
      this->mesh_ready_at_ == 0 ||
      static_cast<uint32_t>(now - this->mesh_ready_at_) < AUTO_ADDRESS_RECOVERY_DELAY_MS ||
      !this->identity_found_this_boot_.load() ||
      this->mesh_rx_messages_.load() != 0 ||
      this->mesh_tx_accepted_.load() < AUTO_ADDRESS_MIN_ACCEPTED_TX ||
      this->mesh_timeouts_.load() < AUTO_ADDRESS_MIN_TIMEOUTS ||
      this->cloud_busy_.load() || this->reboot_pending_.load() ||
      this->composition_query_in_flight_.load() ||
      this->access_operation_.load() != AccessOperation::NONE)
    return;

  this->address_recovery_attempted_this_boot_ = true;
  std::string error;
  if (!this->rotate_local_address_(error)) {
    ESP_LOGE(TAG, "Automatic local Mesh address recovery stopped: %s", error.c_str());
    this->set_status_(error);
  }
}

void NightmatiqMesh::keys_bound_() {
  // A newly imported value has not been authenticated yet, so give the stack
  // time to recover a newer IV Index from a Secure Network Beacon. Once an
  // authenticated Access response (or beacon update) has confirmed it, retain
  // that fact across gateway restarts and start normal polling promptly.
  const bool previously_confirmed =
      (this->config_.flags & FLAG_IV_INDEX_CONFIRMED) != 0;
  this->keys_bound_pending_ = true;
  this->keys_bound_at_ = millis() + (previously_confirmed ? 1000 : 15000);
  this->set_status_(previously_confirmed
                        ? "Mesh keys loaded; restoring confirmed IV Index"
                        : "Mesh keys loaded; synchronizing IV Index");
}

void NightmatiqMesh::provisioning_callback(esp_ble_mesh_prov_cb_event_t event,
                                           esp_ble_mesh_prov_cb_param_t *param) {
  NightmatiqMesh *self = NightmatiqMesh::instance_;
  if (self == nullptr || param == nullptr)
    return;

  switch (event) {
    case ESP_BLE_MESH_PROVISIONER_SET_PRIMARY_ELEM_ADDR_COMP_EVT: {
      const int address_error = param->provisioner_set_primary_elem_addr_comp.err_code;
      if (address_error != 0) {
        ESP_LOGE(TAG, "Bluetooth Mesh provisioner address synchronization failed: %d", address_error);
        self->set_status_("Bluetooth Mesh provisioner address synchronization failed: " +
                          std::to_string(address_error));
        return;
      }

      const uint16_t actual_address = esp_ble_mesh_get_primary_element_address();
      ESP_LOGI(TAG, "Bluetooth Mesh local primary element: 0x%04X", actual_address);
      if (actual_address != self->config_.local_address) {
        ESP_LOGE(TAG, "Bluetooth Mesh local address mismatch: expected 0x%04X", self->config_.local_address);
        self->set_status_("Bluetooth Mesh local address mismatch");
        return;
      }

      self->set_status_("Enabling Bluetooth Mesh bearer");
      const esp_err_t result = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV);
      if (result != ESP_OK) {
        ESP_LOGE(TAG, "Mesh advertising bearer enable failed: %s", esp_err_to_name(result));
        self->set_status_(std::string("Mesh advertising bearer enable failed: ") + esp_err_to_name(result));
        return;
      }
      // The provisioner creates its reserved primary NetKey asynchronously.
      // Import the Steinel key only after PROV_ENABLE_COMP_EVT because
      // add_local_net_key explicitly rejects the reserved primary index (0).
      self->set_status_("Waiting for Bluetooth Mesh provisioner");
      break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT: {
      const int enable_error = param->provisioner_prov_enable_comp.err_code;
      if (enable_error != 0) {
        ESP_LOGE(TAG, "Bluetooth Mesh provisioner enable failed: %d", enable_error);
        self->set_status_("Bluetooth Mesh provisioner enable failed: " + std::to_string(enable_error));
        return;
      }

      self->set_status_("Importing primary Mesh key");
      const uint16_t net_idx = self->config_.net_key_index;
      const uint8_t *existing_net_key = esp_ble_mesh_provisioner_get_local_net_key(net_idx);
      // ESP-IDF reserves index 0 as ESP_BLE_MESH_KEY_PRIMARY and forbids
      // add_local_net_key() for it. The primary key always exists after the
      // provisioner-enable completion event, so replace it with the Steinel
      // key. Non-primary networks retain normal add/update semantics.
      const esp_err_t result =
          net_idx == ESP_BLE_MESH_KEY_PRIMARY || existing_net_key != nullptr
              ? esp_ble_mesh_provisioner_update_local_net_key(self->config_.net_key.data(), net_idx)
              : esp_ble_mesh_provisioner_add_local_net_key(self->config_.net_key.data(), net_idx);
      if (result != ESP_OK) {
        ESP_LOGE(TAG, "Primary NetKey import request failed: %s", esp_err_to_name(result));
        self->set_status_(std::string("Primary NetKey import request failed: ") + esp_err_to_name(result));
      }
      break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_NET_KEY_COMP_EVT:
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_NET_KEY_COMP_EVT: {
      const int error = event == ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_NET_KEY_COMP_EVT
                            ? param->provisioner_add_net_key_comp.err_code
                            : param->provisioner_update_net_key_comp.err_code;
      if (error != 0) {
        ESP_LOGE(TAG, "Primary NetKey import failed: %d", error);
        return;
      }
      // Provisioner callbacks execute in the ESP-BLE-MESH task. Restore the
      // target entry here, after the NetKey exists, rather than touching the
      // private provisioner table from ESPHome's main loop.
      if (!self->restore_target_node_())
        return;
      const uint8_t *existing_app_key =
          esp_ble_mesh_provisioner_get_local_app_key(self->config_.net_key_index, self->config_.app_key_index);
      const esp_err_t result = existing_app_key == nullptr
                                   ? esp_ble_mesh_provisioner_add_local_app_key(
                                         self->config_.app_key.data(), self->config_.net_key_index,
                                         self->config_.app_key_index)
                                   : esp_ble_mesh_provisioner_update_local_app_key(
                                         self->config_.app_key.data(), self->config_.net_key_index,
                                         self->config_.app_key_index);
      if (result != ESP_OK)
        ESP_LOGE(TAG, "AppKey import request failed: %s", esp_err_to_name(result));
      break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
      if (param->provisioner_add_app_key_comp.err_code != 0) {
        ESP_LOGE(TAG, "AppKey import failed: %d", param->provisioner_add_app_key_comp.err_code);
        return;
      }
      self->bind_model_(ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI);
      break;
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_APP_KEY_COMP_EVT:
      if (param->provisioner_update_app_key_comp.err_code != 0) {
        ESP_LOGE(TAG, "AppKey update failed: %d", param->provisioner_update_app_key_comp.err_code);
        return;
      }
      self->bind_model_(ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI);
      break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT: {
      const auto &binding = param->provisioner_bind_app_key_to_model_comp;
      if (binding.err_code != 0) {
        ESP_LOGE(TAG, "AppKey bind failed for model 0x%04X: %d", binding.model_id, binding.err_code);
        return;
      }
      if (binding.model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI)
        self->bind_model_(ESP_BLE_MESH_MODEL_ID_SENSOR_CLI);
      else if (binding.model_id == ESP_BLE_MESH_MODEL_ID_SENSOR_CLI)
        self->bind_model_(ESP_BLE_MESH_MODEL_ID_SCENE_CLI);
      else if (binding.model_id == ESP_BLE_MESH_MODEL_ID_SCENE_CLI)
        self->bind_model_(ESP_BLE_MESH_MODEL_ID_LIGHT_LC_CLI);
      else if (binding.model_id == ESP_BLE_MESH_MODEL_ID_LIGHT_LC_CLI)
        self->keys_bound_();
      break;
    }
    default:
      break;
  }
}

void NightmatiqMesh::config_callback(esp_ble_mesh_cfg_client_cb_event_t event,
                                     esp_ble_mesh_cfg_client_cb_param_t *param) {
  NightmatiqMesh *self = NightmatiqMesh::instance_;
  if (self == nullptr || param == nullptr)
    return;

  // Retain the exact completion reason in the lightweight HTTP diagnostics.
  // This avoids attaching an API log client on RAM-constrained ESP32 builds.
  self->composition_last_event_.store(static_cast<int32_t>(event));
  self->composition_last_error_.store(param->error_code);
  self->composition_last_opcode_.store(
      param->params == nullptr ? 0 : param->params->opcode);
  if (param->params == nullptr ||
      param->params->opcode != ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET)
    return;

  self->composition_query_in_flight_.store(false);
  if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
    self->mesh_timeouts_.fetch_add(1);
    self->composition_timeouts_.fetch_add(1);
    const uint32_t failures = self->composition_query_failures_.fetch_add(1) + 1;
    ESP_LOGW(TAG, "Live NightmatIQ Composition Data request timed out");
    self->composition_query_at_.store(
        millis() + (failures < COMPOSITION_FAST_RETRY_LIMIT
                        ? COMPOSITION_FAST_RETRY_MS
                        : COMPOSITION_BACKGROUND_RETRY_MS));
    self->composition_query_pending_.store(true);
    return;
  }
  if (event != ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT || param->error_code != 0) {
    const uint32_t failures = self->composition_query_failures_.fetch_add(1) + 1;
    ESP_LOGW(TAG, "Live NightmatIQ Composition Data request failed: %d", param->error_code);
    self->composition_query_at_.store(
        millis() + (failures < COMPOSITION_FAST_RETRY_LIMIT
                        ? COMPOSITION_FAST_RETRY_MS
                        : COMPOSITION_BACKGROUND_RETRY_MS));
    self->composition_query_pending_.store(true);
    return;
  }

  const auto &status = param->status_cb.comp_data_status;
  const net_buf_simple *data = status.composition_data;
  if (status.page != 0 || data == nullptr || data->len < 10) {
    self->composition_query_failures_.fetch_add(1);
    self->composition_query_at_.store(millis() + COMPOSITION_BACKGROUND_RETRY_MS);
    self->composition_query_pending_.store(true);
    ESP_LOGW(TAG, "NightmatIQ returned unsupported Composition Data page %u (%u bytes)",
             status.page, data == nullptr ? 0U : static_cast<unsigned>(data->len));
    return;
  }
  const auto read_le16 = [](const uint8_t *value) -> uint16_t {
    return static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8);
  };
  const uint16_t company_id = read_le16(data->data);
  const uint16_t product_id = read_le16(data->data + 2);
  const uint16_t version_id = read_le16(data->data + 4);
  self->live_company_id_.store(company_id);
  self->live_product_id_.store(product_id);
  self->live_version_id_.store(version_id);
  self->composition_received_.store(true);
  self->composition_query_failures_.store(0);
  self->composition_query_at_.store(millis() + COMPOSITION_REFRESH_INTERVAL_MS);
  self->composition_query_pending_.store(true);
  self->composition_responses_.fetch_add(1);
  self->mesh_rx_messages_.fetch_add(1);
  self->record_mesh_rssi_(param->params->ctx);

  if (self->advertised_identity_valid_.load() && self->identity_found_this_boot_.load()) {
    // Associate the observed Steinel manufacturer advertisement with the
    // authenticated Composition VID before retaining it across restarts.
    self->advertised_composition_version_id_.store(version_id);
    self->advertised_identity_save_pending_.store(true);
  } else if (self->advertised_identity_valid_.load() &&
             (self->advertised_composition_version_id_.load() != version_id ||
              self->advertised_product_id_.load() != product_id)) {
    // Never present retained identity metadata for a different composition.
  }
  self->advertised_identity_publish_pending_.store(true);
  ESP_LOGI(TAG, "Live NightmatIQ identity: CID=0x%04X PID=0x%04X VID=0x%04X",
           company_id, product_id, version_id);
}

uint8_t NightmatiqMesh::next_tid_() { return ++this->tid_; }

bool NightmatiqMesh::send_composition_get_() {
  if (!this->mesh_ready_.load() || !this->device_key_valid_ || config_client.model == nullptr)
    return false;
  // Keep this invariant local to the operation as well. It protects later
  // enable/disable cycles from stale ESP-IDF model settings without erasing
  // any user configuration or requiring a gateway restart.
  config_client.model->keys[0] = ESP_BLE_MESH_KEY_DEV;
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_cfg_client_get_state_t get{};
  common.opcode = ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET;
  common.model = config_client.model;
  common.ctx.net_idx = this->config_.net_key_index;
  common.ctx.app_idx = ESP_BLE_MESH_KEY_DEV;
  common.ctx.addr = this->config_.onoff_address;
  common.ctx.send_ttl = MESSAGE_TTL;
  common.msg_timeout = COMPOSITION_MESSAGE_TIMEOUT_MS;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
  common.msg_role = ROLE_PROVISIONER;
#endif
  get.comp_data_get.page = 0;
  this->composition_query_attempts_.fetch_add(1);
  const bool accepted = this->record_send_result_(esp_ble_mesh_config_client_get_state(&common, &get));
  this->composition_query_in_flight_.store(accepted);
  if (accepted)
    this->composition_query_deadline_.store(millis() + COMPOSITION_REQUEST_WATCHDOG_MS);
  return accepted;
}

bool NightmatiqMesh::send_sensor_get_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_sensor_client_get_state_t get{};
  if (!this->set_common_(common, sensor_client.model, ESP_BLE_MESH_MODEL_OP_SENSOR_GET,
                         this->config_.sensor_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::SENSOR_GET,
                                     ESP_BLE_MESH_MODEL_OP_SENSOR_GET))
    return false;
  get.sensor_get.op_en = true;
  get.sensor_get.property_id = AMBIENT_LIGHT_LEVEL_PROPERTY;
  return this->record_access_send_result_(
      AccessOperation::SENSOR_GET, ESP_BLE_MESH_MODEL_OP_SENSOR_GET,
      esp_ble_mesh_sensor_client_get_state(&common, &get));
}

bool NightmatiqMesh::send_device_revision_catalog_get_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_sensor_client_get_state_t get{};
  if (!this->set_common_(common, sensor_client.model, ESP_BLE_MESH_MODEL_OP_SENSOR_GET,
                         this->config_.sensor_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::REVISION_CATALOG_GET,
                                     ESP_BLE_MESH_MODEL_OP_SENSOR_GET))
    return false;
  // A Sensor Get without a Property ID requests every sensor value exposed by
  // the server. One authenticated response can therefore prove whether the
  // standard firmware and hardware revision properties exist, without
  // treating two unanswered optional-property requests as evidence.
  get.sensor_get.op_en = false;
  const bool accepted = this->record_access_send_result_(
      AccessOperation::REVISION_CATALOG_GET, ESP_BLE_MESH_MODEL_OP_SENSOR_GET,
      esp_ble_mesh_sensor_client_get_state(&common, &get));
  this->revision_catalog_in_flight_.store(accepted);
  return accepted;
}

bool NightmatiqMesh::send_threshold_get_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_light_client_get_state_t get{};
  if (!this->set_common_(common, light_lc_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET,
                         this->config_.lc_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::THRESHOLD_GET,
                                     ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET))
    return false;
  get.lc_property_get.property_id = LC_LIGHT_ON_THRESHOLD_PROPERTY;
  return this->record_access_send_result_(
      AccessOperation::THRESHOLD_GET, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_GET,
      esp_ble_mesh_light_client_get_state(&common, &get));
}

bool NightmatiqMesh::send_lc_mode_get_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_light_client_get_state_t get{};
  if (!this->set_common_(common, light_lc_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET,
                         this->config_.lc_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::LC_MODE_GET,
                                     ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET))
    return false;
  return this->record_access_send_result_(
      AccessOperation::LC_MODE_GET, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_GET,
      esp_ble_mesh_light_client_get_state(&common, &get));
}

bool NightmatiqMesh::send_onoff_get_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_generic_client_get_state_t get{};
  // The primary element contains both Generic OnOff Server and Light
  // Lightness Server. Its Generic OnOff state is therefore the direct state
  // of the physical light output, not a state inferred from our commands.
  if (!this->set_common_(common, onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET,
                         this->config_.onoff_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::ONOFF_GET,
                                     ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET))
    return false;
  return this->record_access_send_result_(
      AccessOperation::ONOFF_GET, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET,
      esp_ble_mesh_generic_client_get_state(&common, &get));
}

bool NightmatiqMesh::send_scene_get_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_time_scene_client_get_state_t get{};
  if (!this->set_common_(common, scene_client.model, ESP_BLE_MESH_MODEL_OP_SCENE_GET,
                         this->config_.onoff_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::SCENE_GET,
                                     ESP_BLE_MESH_MODEL_OP_SCENE_GET))
    return false;
  return this->record_access_send_result_(
      AccessOperation::SCENE_GET, ESP_BLE_MESH_MODEL_OP_SCENE_GET,
      esp_ble_mesh_time_scene_client_get_state(&common, &get));
}

bool NightmatiqMesh::send_threshold_set_(uint32_t centilux) {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_light_client_set_state_t set{};
  if (!this->set_common_(common, light_lc_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET,
                         this->config_.lc_address))
    return false;
  if (!this->begin_access_operation_(AccessOperation::THRESHOLD_SET,
                                     ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET))
    return false;
  // Light LC Ambient LuxLevel On uses the Bluetooth Mesh 24-bit Illuminance
  // format (0.01 lx resolution), not a 32-bit integer. ESP-IDF does not deep
  // copy property_value, so its storage must outlive this function.
  this->threshold_set_storage_[0] = centilux & 0xFF;
  this->threshold_set_storage_[1] = (centilux >> 8) & 0xFF;
  this->threshold_set_storage_[2] = (centilux >> 16) & 0xFF;
  this->threshold_set_buffer_.data = this->threshold_set_storage_.data();
  this->threshold_set_buffer_.len = this->threshold_set_storage_.size();
  this->threshold_set_buffer_.size = this->threshold_set_storage_.size();
  this->threshold_set_buffer_.__buf = this->threshold_set_storage_.data();
  set.lc_property_set.property_id = LC_LIGHT_ON_THRESHOLD_PROPERTY;
  set.lc_property_set.property_value = &this->threshold_set_buffer_;
  return this->record_access_send_result_(
      AccessOperation::THRESHOLD_SET, ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_SET,
      esp_ble_mesh_light_client_set_state(&common, &set));
}

bool NightmatiqMesh::send_lc_mode_set_(bool enabled) {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_light_client_set_state_t set{};
  if (!this->set_common_(common, light_lc_client.model,
                         ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_SET_UNACK,
                         this->config_.lc_address))
    return false;
  set.lc_mode_set.mode = enabled ? 1 : 0;
  return this->record_send_result_(esp_ble_mesh_light_client_set_state(&common, &set));
}

bool NightmatiqMesh::send_onoff_set_(bool on) {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_generic_client_set_state_t set{};
  if (!this->set_common_(common, onoff_client.model,
                         ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK,
                         this->config_.onoff_address))
    return false;
  set.onoff_set.op_en = false;
  set.onoff_set.onoff = on ? 1 : 0;
  // Repeated copies of one logical command must use the same TID so the
  // server treats them as retransmissions, not as separate state changes.
  set.onoff_set.tid = this->control_tid_;
  return this->record_send_result_(esp_ble_mesh_generic_client_set_state(&common, &set));
}

bool NightmatiqMesh::send_scene_recall_() {
  esp_ble_mesh_client_common_param_t common{};
  esp_ble_mesh_time_scene_client_set_state_t set{};
  if (!this->set_common_(common, scene_client.model,
                         ESP_BLE_MESH_MODEL_OP_SCENE_RECALL_UNACK,
                         this->config_.onoff_address))
    return false;
  set.scene_recall.op_en = false;
  set.scene_recall.scene_number = this->config_.scene_number;
  set.scene_recall.tid = this->control_tid_;
  return this->record_send_result_(esp_ble_mesh_time_scene_client_set_state(&common, &set));
}

void NightmatiqMesh::set_threshold(float lux) {
  if (!this->mesh_ready_.load()) {
    ESP_LOGW(TAG, "Threshold ignored because the mesh client is not ready yet");
    return;
  }
  if (!std::isfinite(lux) || lux < 1.0f || lux > 1500.0f) {
    ESP_LOGW(TAG, "Rejected invalid twilight threshold: %.2f lx", lux);
    if (this->threshold_received_.load()) {
      this->pending_threshold_centilux_.store(this->observed_threshold_centilux_.load());
      this->threshold_publish_pending_.store(true);
    }
    return;
  }
  const uint32_t centilux = static_cast<uint32_t>(std::lround(lux * 100.0f));
  this->requested_threshold_centilux_.store(centilux);
  this->threshold_override_pending_.store(true);
  this->threshold_request_sequence_.fetch_add(1);
  // HA sees the requested value immediately. It is kept only when the device
  // confirms it; otherwise finish_control_ restores the last verified value.
  this->pending_threshold_centilux_.store(centilux);
  this->threshold_publish_pending_.store(true);
}

void NightmatiqMesh::set_mode(const std::string &mode) {
  if (!this->mesh_ready_.load()) {
    ESP_LOGW(TAG, "Mode command ignored because the mesh client is not ready yet");
    return;
  }

  int8_t requested = -1;
  if (mode == "Auto")
    requested = 0;
  else if (mode == "Always On")
    requested = 1;
  else if (mode == "Always Off")
    requested = 2;
  else {
    ESP_LOGW(TAG, "Unknown NightmatIQ mode: %s", mode.c_str());
    return;
  }
  this->requested_mode_.store(requested);
  this->mode_override_pending_.store(true);
  this->mode_confirmation_deadline_.store(0);
  this->mode_request_sequence_.fetch_add(1);
  // Do not let an older poll response make the selector jump back while this
  // acknowledged two-step command is still being applied and verified.
  this->mode_publish_pending_.store(requested);
}

bool NightmatiqMesh::control_request_pending_() const {
  return this->mode_request_sequence_.load() != this->handled_mode_request_sequence_ ||
         this->threshold_request_sequence_.load() != this->handled_threshold_request_sequence_;
}

void NightmatiqMesh::start_next_control_(uint32_t now) {
  if (this->control_kind_ != ControlKind::NONE)
    return;

  if (this->mode_request_sequence_.load() != this->handled_mode_request_sequence_) {
    this->handled_mode_request_sequence_ = this->mode_request_sequence_.load();
    this->control_kind_ = ControlKind::MODE;
    this->control_step_ = ControlStep::MODE_SEND_LC;
    this->control_tid_ = this->next_tid_();
  } else if (this->threshold_request_sequence_.load() !=
             this->handled_threshold_request_sequence_) {
    if (this->access_operation_.load() != AccessOperation::NONE)
      return;
    this->handled_threshold_request_sequence_ = this->threshold_request_sequence_.load();
    this->control_kind_ = ControlKind::THRESHOLD;
    this->control_step_ = ControlStep::THRESHOLD_SEND_SET;
    this->control_previous_threshold_centilux_ =
        this->threshold_received_.load() ? this->observed_threshold_centilux_.load() : 0;
  } else {
    return;
  }

  this->control_attempt_ = 0;
  this->control_action_at_ = now;
  this->poll_stage_ = 0;
}

void NightmatiqMesh::finish_control_(bool success, uint32_t now) {
  const ControlKind completed = this->control_kind_;
  const bool newer_mode = completed == ControlKind::MODE &&
                          this->mode_request_sequence_.load() !=
                              this->handled_mode_request_sequence_;
  const bool newer_threshold = completed == ControlKind::THRESHOLD &&
                               this->threshold_request_sequence_.load() !=
                                   this->handled_threshold_request_sequence_;

  if (completed == ControlKind::MODE && !newer_mode) {
    if (success) {
      // Unacknowledged messages provide immediate physical control. Keep HA's
      // optimistic selection until the following GETs confirm it or the grace
      // period expires; a stale poll response must not make the selector jump.
      this->mode_confirmation_deadline_.store(now + MODE_CONFIRMATION_GRACE_MS);
      this->mode_publish_pending_.store(this->requested_mode_.load());
      this->poll_sensor_rx_start_ = this->mesh_sensor_rx_.load();
      this->poll_generic_rx_start_ = this->mesh_generic_rx_.load();
      this->output_confirmation_pending_ = true;
      this->output_confirmation_attempts_ = 0;
      this->poll_stage_ = 3;
      this->poll_stage_at_ = now + 500;
    } else {
      this->output_confirmation_pending_ = false;
      this->output_confirmation_attempts_ = 0;
      this->mode_override_pending_.store(false);
      this->mode_confirmation_deadline_.store(0);
      this->publish_mode_from_observed_();
    }
  } else if (completed == ControlKind::THRESHOLD && !newer_threshold) {
    this->threshold_override_pending_.store(false);
    if (success) {
      this->pending_threshold_centilux_.store(this->requested_threshold_centilux_.load());
      this->threshold_publish_pending_.store(true);
    } else if (this->control_previous_threshold_centilux_ >= 100 &&
               this->control_previous_threshold_centilux_ <= 100000) {
      this->pending_threshold_centilux_.store(this->control_previous_threshold_centilux_);
      this->threshold_publish_pending_.store(true);
    } else {
      this->threshold_invalidate_pending_.store(true);
    }
  }

  this->control_kind_ = ControlKind::NONE;
  this->control_step_ = ControlStep::IDLE;
  this->control_action_at_ = now + 300;
}

void NightmatiqMesh::retry_or_finish_control_(bool success, uint32_t now) {
  if (success) {
    this->finish_control_(true, now);
    return;
  }
  if (this->control_attempt_ < 2) {
    this->control_attempt_++;
    this->control_step_ = this->control_kind_ == ControlKind::MODE
                              ? ControlStep::MODE_SEND_LC
                              : ControlStep::THRESHOLD_SEND_SET;
    this->control_action_at_ = now + 500;
    ESP_LOGW(TAG, "NightmatIQ control transaction retry %u of 2",
             static_cast<unsigned>(this->control_attempt_));
    return;
  }
  ESP_LOGE(TAG, "NightmatIQ control transaction was not confirmed");
  this->finish_control_(false, now);
}

void NightmatiqMesh::advance_control_(uint32_t now) {
  if (this->control_kind_ == ControlKind::NONE) {
    this->start_next_control_(now);
    return;
  }
  if ((this->control_kind_ != ControlKind::MODE &&
       this->access_operation_.load() != AccessOperation::NONE) ||
      static_cast<int32_t>(now - this->control_action_at_) < 0)
    return;

  // A newer value of the same entity supersedes the old transaction. Wait
  // for its current acknowledged message to finish, then restart from step 1.
  if (this->control_kind_ == ControlKind::MODE &&
      this->mode_request_sequence_.load() != this->handled_mode_request_sequence_) {
    this->handled_mode_request_sequence_ = this->mode_request_sequence_.load();
    this->control_attempt_ = 0;
    this->control_tid_ = this->next_tid_();
    this->control_step_ = ControlStep::MODE_SEND_LC;
  } else if (this->control_kind_ == ControlKind::THRESHOLD &&
             this->threshold_request_sequence_.load() !=
                 this->handled_threshold_request_sequence_) {
    this->handled_threshold_request_sequence_ = this->threshold_request_sequence_.load();
    this->control_attempt_ = 0;
    this->control_step_ = ControlStep::THRESHOLD_SEND_SET;
  }

  const int8_t requested_mode = this->requested_mode_.load();
  const uint32_t requested_threshold = this->requested_threshold_centilux_.load();
  switch (this->control_step_) {
    case ControlStep::MODE_SEND_LC:
      if (!this->send_lc_mode_set_(requested_mode == 0)) {
        this->retry_or_finish_control_(false, now);
      } else {
        this->control_step_ = ControlStep::MODE_SEND_ACTION;
        this->control_action_at_ = now + FAST_CONTROL_STEP_MS;
      }
      break;
    case ControlStep::MODE_SEND_ACTION: {
      const bool sent = requested_mode == 0 ? this->send_scene_recall_()
                                           : this->send_onoff_set_(requested_mode == 1);
      if (!sent) {
        this->retry_or_finish_control_(false, now);
      } else {
        this->control_step_ = ControlStep::MODE_REPEAT_LC;
        this->control_action_at_ = now + FAST_CONTROL_STEP_MS;
      }
      break;
    }
    case ControlStep::MODE_REPEAT_LC:
      if (!this->send_lc_mode_set_(requested_mode == 0)) {
        this->retry_or_finish_control_(false, now);
      } else {
        this->control_step_ = ControlStep::MODE_REPEAT_ACTION;
        this->control_action_at_ = now + FAST_CONTROL_STEP_MS;
      }
      break;
    case ControlStep::MODE_REPEAT_ACTION: {
      const bool sent = requested_mode == 0 ? this->send_scene_recall_()
                                           : this->send_onoff_set_(requested_mode == 1);
      this->retry_or_finish_control_(sent, now);
      break;
    }
    case ControlStep::THRESHOLD_SEND_SET:
      this->control_response_sequence_ = this->threshold_response_sequence_.load();
      if (this->send_threshold_set_(requested_threshold))
        this->control_step_ = ControlStep::THRESHOLD_WAIT_SET;
      else
        this->retry_or_finish_control_(false, now);
      break;
    case ControlStep::THRESHOLD_WAIT_SET:
      if (this->access_last_completed_.load() != AccessOperation::THRESHOLD_SET ||
          !this->access_last_success_.load() ||
          this->threshold_response_sequence_.load() == this->control_response_sequence_ ||
          this->observed_threshold_centilux_.load() != requested_threshold) {
        this->retry_or_finish_control_(false, now);
      } else {
        this->retry_or_finish_control_(true, now);
      }
      break;
    default:
      this->finish_control_(false, now);
      break;
  }
}

void NightmatiqMesh::publish_mode_from_observed_() {
  const int8_t lc_mode = this->observed_lc_mode_.load();
  const int8_t onoff = this->observed_onoff_.load();
  if (this->mode_override_pending_.load()) {
    const int8_t requested = this->requested_mode_.load();
    const bool confirmed = requested == 0 ? lc_mode == 1
                                          : lc_mode == 0 && onoff >= 0 &&
                                                onoff == (requested == 1 ? 1 : 0);
    if (confirmed) {
      this->mode_override_pending_.store(false);
      this->mode_confirmation_deadline_.store(0);
      this->mode_publish_pending_.store(requested);
      return;
    }
    const uint32_t deadline = this->mode_confirmation_deadline_.load();
    if (deadline == 0 || static_cast<int32_t>(millis() - deadline) < 0)
      return;
    this->mode_override_pending_.store(false);
    this->mode_confirmation_deadline_.store(0);
  }
  if (lc_mode == 1)
    this->mode_publish_pending_.store(0);
  else if (lc_mode == 0 && onoff >= 0)
    this->mode_publish_pending_.store(onoff != 0 ? 1 : 2);
}

void NightmatiqMesh::record_actual_output_(bool on) {
  if (this->actual_output_forced_unavailable_.load())
    return;
  this->observed_onoff_.store(on ? 1 : 0);
  this->actual_output_last_response_at_.store(millis());
  this->actual_output_publish_pending_.store(true);
  this->publish_mode_from_observed_();
}

void NightmatiqMesh::force_actual_output_unavailable_() {
  this->actual_output_forced_unavailable_.store(true);
  this->actual_output_publish_pending_.store(false);
  this->actual_output_invalidate_pending_.store(true);
}

void NightmatiqMesh::record_mesh_rssi_(const esp_ble_mesh_msg_ctx_t &context) {
  this->last_mesh_rssi_dbm_.store(context.recv_rssi);
  this->last_mesh_rssi_at_.store(millis());
  this->mesh_rssi_received_.store(true);
  this->mesh_rssi_publish_pending_.store(true);
}

void NightmatiqMesh::generic_callback(esp_ble_mesh_generic_client_cb_event_t event,
                                      esp_ble_mesh_generic_client_cb_param_t *param) {
  NightmatiqMesh *self = NightmatiqMesh::instance_;
  if (self == nullptr || param == nullptr || param->params == nullptr)
    return;
  if (event == ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT) {
    self->mesh_timeouts_.fetch_add(1);
    self->mesh_generic_timeouts_.fetch_add(1);
    ESP_LOGW(TAG, "Generic OnOff request to 0x%04X timed out", param->params->ctx.addr);
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  if (param->error_code != 0 ||
      param->params->ctx.recv_op != ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  self->mesh_rx_messages_.fetch_add(1);
  self->mesh_generic_rx_.fetch_add(1);
  self->record_mesh_rssi_(param->params->ctx);
  self->onoff_response_sequence_.fetch_add(1);
  self->record_actual_output_(param->status_cb.onoff_status.present_onoff != 0);
  self->complete_access_operation_(param->params->opcode, true);
}

void NightmatiqMesh::sensor_callback(esp_ble_mesh_sensor_client_cb_event_t event,
                                     esp_ble_mesh_sensor_client_cb_param_t *param) {
  NightmatiqMesh *self = NightmatiqMesh::instance_;
  if (self == nullptr || param == nullptr || param->params == nullptr)
    return;
  if (event == ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT) {
    self->revision_catalog_in_flight_.store(false);
    self->mesh_timeouts_.fetch_add(1);
    self->mesh_sensor_timeouts_.fetch_add(1);
    ESP_LOGW(TAG, "Ambient light request timed out");
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  if (param->error_code != 0 || param->params->ctx.recv_op != ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS) {
    self->revision_catalog_in_flight_.store(false);
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  self->mesh_rx_messages_.fetch_add(1);
  self->mesh_sensor_rx_.fetch_add(1);
  self->record_mesh_rssi_(param->params->ctx);

  net_buf_simple *buffer = param->status_cb.sensor_status.marshalled_sensor_data;
  if (buffer == nullptr) {
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  const bool revision_catalog = self->revision_catalog_in_flight_.exchange(false);
  bool firmware_revision_present = false;
  bool hardware_revision_present = false;
  const uint8_t *data = buffer->data;
  size_t remaining = buffer->len;
  while (remaining > 0) {
    const uint8_t format = ESP_BLE_MESH_GET_SENSOR_DATA_FORMAT(data);
    const size_t mpid_length = format == ESP_BLE_MESH_SENSOR_DATA_FORMAT_A
                                   ? ESP_BLE_MESH_SENSOR_DATA_FORMAT_A_MPID_LEN
                                   : ESP_BLE_MESH_SENSOR_DATA_FORMAT_B_MPID_LEN;
    if (remaining < mpid_length)
      break;
    const uint8_t encoded_length = ESP_BLE_MESH_GET_SENSOR_DATA_LENGTH(data, format);
    const uint16_t property_id = ESP_BLE_MESH_GET_SENSOR_DATA_PROPERTY_ID(data, format);
    firmware_revision_present |= property_id == 0x000E;
    hardware_revision_present |= property_id == 0x0010;
    if (encoded_length == ESP_BLE_MESH_SENSOR_DATA_ZERO_LEN) {
      if (property_id == 0x000E)
        self->live_firmware_revision_.store(-2);
      else if (property_id == 0x0010)
        self->live_hardware_revision_.store(-2);
      data += mpid_length;
      remaining -= mpid_length;
      continue;
    }
    const size_t value_length = static_cast<size_t>(encoded_length) + 1;
    if (remaining < mpid_length + value_length)
      break;
    if (property_id == AMBIENT_LIGHT_LEVEL_PROPERTY && value_length >= 3) {
      const uint8_t *value = data + mpid_length;
      const uint32_t centilux = static_cast<uint32_t>(value[0]) |
                               (static_cast<uint32_t>(value[1]) << 8) |
                               (static_cast<uint32_t>(value[2]) << 16);
      self->pending_lux_centilux_.store(centilux);
      self->lux_received_.store(true);
      self->lux_publish_pending_.store(true);
    } else if (property_id == 0x000E && value_length >= 1) {
      self->live_firmware_revision_.store(data[mpid_length]);
    } else if (property_id == 0x0010 && value_length >= 1) {
      self->live_hardware_revision_.store(data[mpid_length]);
    }
    data += mpid_length + value_length;
    remaining -= mpid_length + value_length;
  }
  if (revision_catalog) {
    if (!firmware_revision_present)
      self->live_firmware_revision_.store(-2);
    if (!hardware_revision_present)
      self->live_hardware_revision_.store(-2);
  }
  self->complete_access_operation_(param->params->opcode, true);
}

void NightmatiqMesh::light_callback(esp_ble_mesh_light_client_cb_event_t event,
                                    esp_ble_mesh_light_client_cb_param_t *param) {
  NightmatiqMesh *self = NightmatiqMesh::instance_;
  if (self == nullptr || param == nullptr || param->params == nullptr)
    return;
  if (event == ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT) {
    self->mesh_timeouts_.fetch_add(1);
    self->mesh_light_timeouts_.fetch_add(1);
    ESP_LOGW(TAG, "Light LC request opcode 0x%08" PRIX32 " timed out", param->params->opcode);
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  if (param->error_code != 0) {
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  const uint32_t received = param->params->ctx.recv_op;
  const bool recognized = received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS ||
                          received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS ||
                          received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS;
  if (!recognized) {
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  self->mesh_rx_messages_.fetch_add(1);
  self->mesh_light_rx_.fetch_add(1);
  self->record_mesh_rssi_(param->params->ctx);

  if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_MODE_STATUS) {
    self->observed_lc_mode_.store(param->status_cb.lc_mode_status.mode ? 1 : 0);
    self->lc_mode_response_sequence_.fetch_add(1);
    self->publish_mode_from_observed_();
  } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_LIGHT_ONOFF_STATUS) {
    self->record_actual_output_(param->status_cb.lc_light_onoff_status.present_light_onoff != 0);
  } else if (received == ESP_BLE_MESH_MODEL_OP_LIGHT_LC_PROPERTY_STATUS) {
    const auto &status = param->status_cb.lc_property_status;
    if (status.property_id == LC_LIGHT_ON_THRESHOLD_PROPERTY && status.property_value != nullptr &&
        status.property_value->len >= 3) {
      const uint8_t *value = status.property_value->data;
      const uint32_t centilux = static_cast<uint32_t>(value[0]) |
                               (static_cast<uint32_t>(value[1]) << 8) |
                               (static_cast<uint32_t>(value[2]) << 16);
      // The public HA entity supports 1..1500 lx. Reject every out-of-range
      // 24-bit property value before publishing or retaining it.
      if (centilux >= 100 && centilux <= 150000) {
        self->observed_threshold_centilux_.store(centilux);
        self->threshold_response_sequence_.fetch_add(1);
        self->threshold_received_.store(true);
        if (!self->threshold_override_pending_.load() ||
            centilux == self->requested_threshold_centilux_.load()) {
          self->pending_threshold_centilux_.store(centilux);
          self->threshold_publish_pending_.store(true);
        }
      } else {
        ESP_LOGE(TAG, "Rejected invalid NightmatIQ threshold: %.2f lx",
                 centilux / 100.0f);
      }
    }
  }
  self->complete_access_operation_(param->params->opcode, true);
}

void NightmatiqMesh::scene_callback(esp_ble_mesh_time_scene_client_cb_event_t event,
                                    esp_ble_mesh_time_scene_client_cb_param_t *param) {
  NightmatiqMesh *self = NightmatiqMesh::instance_;
  if (self == nullptr || param == nullptr || param->params == nullptr)
    return;
  if (event == ESP_BLE_MESH_TIME_SCENE_CLIENT_TIMEOUT_EVT) {
    self->mesh_timeouts_.fetch_add(1);
    self->mesh_scene_timeouts_.fetch_add(1);
    ESP_LOGW(TAG, "Scene request timed out");
    self->complete_access_operation_(param->params->opcode, false);
    return;
  }
  const bool recognized = param->error_code == 0 &&
                          param->params->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_SCENE_STATUS;
  if (recognized) {
    self->mesh_rx_messages_.fetch_add(1);
    self->mesh_scene_rx_.fetch_add(1);
    self->record_mesh_rssi_(param->params->ctx);
  }
  const bool success = recognized && param->status_cb.scene_status.status_code == 0;
  self->complete_access_operation_(param->params->opcode, success);
}

void NightmatiqMesh::publish_pending_() {
  if (this->status_publish_pending_.exchange(false) && this->status_text_sensor_ != nullptr) {
    std::string status;
    {
      std::lock_guard<std::mutex> lock(this->state_mutex_);
      status = this->status_;
    }
    this->status_text_sensor_->publish_state(status);
  }
  if (this->advertised_identity_publish_pending_.exchange(false)) {
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;
    if (this->resolve_firmware_version_(major, minor, patch)) {
      char firmware[16];
      std::snprintf(firmware, sizeof(firmware), "%u.%u.%u", major, minor, patch);
      if (this->firmware_version_text_sensor_ != nullptr)
        this->firmware_version_text_sensor_->publish_state(firmware);
    } else if (this->firmware_version_text_sensor_ != nullptr) {
      this->firmware_version_text_sensor_->publish_state("");
    }

    const bool hardware_known = this->advertised_identity_current_();
    const uint8_t hardware = this->advertised_hardware_version_.load();
    if (this->hardware_version_sensor_ != nullptr)
      this->hardware_version_sensor_->publish_state(hardware_known ? hardware : NAN);

    if (this->composition_received_.load()) {
      const uint16_t company_id = this->live_company_id_.load();
      const uint16_t product_id = this->live_product_id_.load();
      char company[40];
      char product[16];
      if (company_id == STEINEL_COMPANY_ID) {
        std::snprintf(company, sizeof(company), "0x%04x (Steinel GmbH)", company_id);
        if (this->manufacturer_text_sensor_ != nullptr)
          this->manufacturer_text_sensor_->publish_state("Steinel GmbH");
      } else {
        std::snprintf(company, sizeof(company), "0x%04x", company_id);
        if (this->manufacturer_text_sensor_ != nullptr)
          this->manufacturer_text_sensor_->publish_state("");
      }
      std::snprintf(product, sizeof(product), "0x%04x", product_id);
      if (this->company_id_text_sensor_ != nullptr)
        this->company_id_text_sensor_->publish_state(company);
      if (this->product_id_text_sensor_ != nullptr)
        this->product_id_text_sensor_->publish_state(product);
    } else {
      if (this->manufacturer_text_sensor_ != nullptr)
        this->manufacturer_text_sensor_->publish_state("");
      if (this->company_id_text_sensor_ != nullptr)
        this->company_id_text_sensor_->publish_state("");
      if (this->product_id_text_sensor_ != nullptr)
        this->product_id_text_sensor_->publish_state("");
    }
  }
  if (this->ready_publish_pending_.exchange(false)) {
    if (this->ready_binary_sensor_ != nullptr)
      this->ready_binary_sensor_->publish_state(this->mesh_ready_.load());
  }
  if (this->actual_output_invalidate_pending_.exchange(false)) {
    if (this->actual_output_binary_sensor_ != nullptr)
      this->actual_output_binary_sensor_->invalidate_state();
    this->actual_output_available_ = false;
  }
  if (this->actual_output_publish_pending_.exchange(false) &&
      !this->actual_output_forced_unavailable_.load()) {
    const int8_t actual_output = this->observed_onoff_.load();
    if (actual_output >= 0 && this->actual_output_binary_sensor_ != nullptr) {
      this->actual_output_binary_sensor_->publish_state(actual_output != 0);
      this->actual_output_available_ = true;
    }
  }
  if (this->actual_output_available_ && !this->actual_output_forced_unavailable_.load() &&
      static_cast<uint32_t>(millis() - this->actual_output_last_response_at_.load()) >=
          ACTUAL_OUTPUT_STALE_MS) {
    if (this->actual_output_binary_sensor_ != nullptr)
      this->actual_output_binary_sensor_->invalidate_state();
    this->actual_output_available_ = false;
  }
  if (this->lux_publish_pending_.exchange(false)) {
    if (this->lux_sensor_ != nullptr)
      this->lux_sensor_->publish_state(this->pending_lux_centilux_.load() / 100.0f);
  }
  if (this->mesh_rssi_publish_pending_.exchange(false)) {
    if (this->rssi_sensor_ != nullptr)
      this->rssi_sensor_->publish_state(this->last_mesh_rssi_dbm_.load());
  }
  if (this->threshold_invalidate_pending_.exchange(false)) {
    if (this->threshold_number_ != nullptr)
      this->threshold_number_->publish_state(NAN);
  }
  if (this->threshold_publish_pending_.exchange(false)) {
    if (this->threshold_number_ != nullptr)
      this->threshold_number_->publish_state(this->pending_threshold_centilux_.load() / 100.0f);
  }
  const int8_t mode = this->mode_publish_pending_.exchange(-1);
  if (mode >= 0 && this->mode_select_ != nullptr) {
    if (mode == 0)
      this->mode_select_->publish_state("Auto");
    else if (mode == 1)
      this->mode_select_->publish_state("Always On");
    else
      this->mode_select_->publish_state("Always Off");
  }
}

void NightmatiqMesh::update() {
  if (!this->mesh_ready_.load() || this->poll_stage_ != 0 ||
      this->access_operation_.load() != AccessOperation::NONE ||
      this->control_kind_ != ControlKind::NONE || this->control_request_pending_())
    return;
  if (this->control_kind_ == ControlKind::NONE && !this->control_request_pending_() &&
      this->access_operation_.load() == AccessOperation::NONE) {
    this->poll_sensor_rx_start_ = this->mesh_sensor_rx_.load();
    this->poll_generic_rx_start_ = this->mesh_generic_rx_.load();
    this->poll_stage_ = 1;
    this->poll_stage_at_ = millis();
  }
}

void NightmatiqMesh::request_refresh() {
  if (!this->mesh_ready_.load()) {
    if (!this->configured_)
      this->set_status_("Configuration required");
    else if (!this->mesh_mode_enabled_)
      this->set_status_("NightmatIQ disabled; gateway in setup mode");
    else
      this->set_status_("Mesh is still synchronizing");
    return;
  }
  if (this->device_key_valid_) {
    this->composition_query_failures_.store(0);
    this->composition_query_at_.store(millis());
    this->composition_query_pending_.store(true);
  }
  this->poll_sensor_rx_start_ = this->mesh_sensor_rx_.load();
  this->poll_generic_rx_start_ = this->mesh_generic_rx_.load();
  this->poll_stage_ = 1;
  this->poll_stage_at_ = millis();
}

void NightmatiqMesh::loop() {
  this->advance_factory_reset_();
  this->persist_pending_advertised_identity_();
  this->publish_pending_();
  if (this->auto_update_mode_) {
    this->advance_auto_update_();
    const uint32_t update_now = millis();
    if (this->reboot_pending_.load() &&
        static_cast<int32_t>(update_now - this->reboot_at_) >= 0) {
      this->reboot_pending_.store(false);
      reboot_after_confirming_firmware();
    }
    return;
  }
  this->persist_address_confirmation_();
  this->advance_identity_scan_();
  this->advance_mesh_start_();
  this->advance_mesh_remove_();
  this->monitor_iv_index_();
  if (this->cloud_ble_pause_pending_.exchange(false)) {
    if (this->identity_scan_pending_.load())
      esp_ble_gap_stop_scanning();
    if (esp32_ble::global_ble != nullptr)
      esp32_ble::global_ble->disable();
  }
  this->advance_cloud_job_();
  this->resume_ble_after_cloud_();

  const uint32_t now = millis();
  if (this->reboot_pending_.load() && static_cast<int32_t>(now - this->reboot_at_) >= 0) {
    this->reboot_pending_.store(false);
    reboot_after_confirming_firmware();
    return;
  }
  if (this->cloud_session_reboot_pending_.load() && !this->cloud_busy_.load() &&
      static_cast<int32_t>(now - this->cloud_session_reboot_at_.load()) >= 0) {
    this->cloud_session_reboot_pending_.store(false);
    reboot_after_confirming_firmware();
    return;
  }
  if (this->keys_bound_pending_ && static_cast<int32_t>(now - this->keys_bound_at_) >= 0) {
    this->keys_bound_pending_ = false;
    this->mark_ready_();
  }
  if (!this->mesh_ready_.load())
    return;
  if (this->web_refresh_pending_.load() &&
      this->access_operation_.load() == AccessOperation::NONE &&
      this->control_kind_ == ControlKind::NONE && !this->control_request_pending_()) {
    this->web_refresh_pending_.store(false);
    this->request_refresh();
  }
  this->expire_access_operation_(now);
  this->advance_address_recovery_(now);
  if (this->reboot_pending_.load())
    return;
  if (this->mode_override_pending_.load() &&
      this->mode_confirmation_deadline_.load() != 0 &&
      static_cast<int32_t>(now - this->mode_confirmation_deadline_.load()) >= 0) {
    this->publish_mode_from_observed_();
  }
  if (this->composition_query_in_flight_.load() &&
      static_cast<int32_t>(now - this->composition_query_deadline_.load()) >= 0) {
    // Some ESP-IDF error completion events do not carry the request opcode and
    // are therefore not attributable in config_callback(). Do not let such an
    // event permanently suppress live identity reads.
    this->composition_query_in_flight_.store(false);
    this->mesh_timeouts_.fetch_add(1);
    this->composition_timeouts_.fetch_add(1);
    const uint32_t failures = this->composition_query_failures_.fetch_add(1) + 1;
    this->composition_query_at_.store(
        now + (failures < COMPOSITION_FAST_RETRY_LIMIT
                   ? COMPOSITION_FAST_RETRY_MS
                   : COMPOSITION_BACKGROUND_RETRY_MS));
    this->composition_query_pending_.store(true);
    ESP_LOGW(TAG, "Live NightmatIQ Composition Data request watchdog expired");
  }

  if (this->composition_query_pending_.load() &&
      !this->composition_query_in_flight_.load() &&
      this->access_operation_.load() == AccessOperation::NONE &&
      this->control_kind_ == ControlKind::NONE && !this->control_request_pending_() &&
      this->poll_stage_ == 0 && static_cast<int32_t>(now - this->composition_query_at_.load()) >= 0) {
    this->composition_query_pending_.store(false);
    if (!this->send_composition_get_()) {
      const uint32_t failures = this->composition_query_failures_.fetch_add(1) + 1;
      this->composition_query_at_.store(
          now + (failures < COMPOSITION_FAST_RETRY_LIMIT
                     ? COMPOSITION_FAST_RETRY_MS
                     : COMPOSITION_BACKGROUND_RETRY_MS));
      this->composition_query_pending_.store(true);
    }
  }
  if (this->composition_query_in_flight_.load() &&
      this->control_kind_ == ControlKind::NONE && !this->control_request_pending_())
    return;

  // Give the authenticated Composition Data request first use of the Config
  // Client so identity is available before the multi-stage state poll.
  const bool initial_identity_attempt_complete =
      !this->device_key_valid_ || this->composition_received_.load() ||
      (this->composition_query_attempts_.load() > 0 &&
       !this->composition_query_in_flight_.load());
  if (!this->initial_poll_started_ && initial_identity_attempt_complete) {
    this->initial_poll_started_ = true;
    this->update();
  }

  if (this->control_kind_ != ControlKind::NONE || this->control_request_pending_()) {
    // User control has priority over background reads. Finish at most the one
    // already active access request, then run and verify the queued command.
    this->poll_stage_ = 0;
    this->advance_control_(now);
    return;
  }

  if (this->access_operation_.load() != AccessOperation::NONE)
    return;

  if (this->poll_stage_ != 0 && static_cast<int32_t>(now - this->poll_stage_at_) >= 0) {
    bool advance = true;
    switch (this->poll_stage_) {
      case 1:
        this->send_sensor_get_();
        break;
      case 2:
        this->send_threshold_get_();
        break;
      case 3:
        // Confirm the safety-relevant physical output before the LC mode.
        // NightmatIQ often omits an LC Mode Status; putting that request first
        // delayed HA output confirmation by a full timeout.
        if (this->send_onoff_get_() && this->output_confirmation_pending_)
          this->output_confirmation_attempts_++;
        break;
      case 4:
        // Give a missed physical-output query its second chance immediately;
        // do not place LC mode, Scene and Sensor timeouts in front of it.
        if (this->mesh_generic_rx_.load() == this->poll_generic_rx_start_ &&
            (!this->output_confirmation_pending_ ||
             this->output_confirmation_attempts_ < OUTPUT_CONFIRMATION_MAX_ATTEMPTS)) {
          if (this->send_onoff_get_() && this->output_confirmation_pending_)
            this->output_confirmation_attempts_++;
        }
        break;
      case 5:
        if (this->output_confirmation_pending_) {
          if (this->mesh_generic_rx_.load() != this->poll_generic_rx_start_) {
            // record_actual_output_ has already queued the fresh state for HA.
            this->output_confirmation_pending_ = false;
            this->output_confirmation_attempts_ = 0;
          } else if (this->output_confirmation_attempts_ <
                     OUTPUT_CONFIRMATION_MAX_ATTEMPTS) {
            // Keep confirmation ahead of slower LC/Scene/Sensor reads. This
            // avoids falling back to the normal 30-second poll after only two
            // lost replies while preserving a real, non-optimistic HA state.
            this->poll_stage_ = 3;
            this->poll_stage_at_ = now + 250;
            advance = false;
            break;
          } else {
            this->output_confirmation_pending_ = false;
            this->output_confirmation_attempts_ = 0;
          }
        }
        this->send_lc_mode_get_();
        break;
      case 6:
        this->send_scene_get_();
        break;
      case 7:
        // The NightmatIQ occasionally misses one acknowledged request even at
        // close range. Retry only when this complete poll cycle has not
        // received any Sensor Status; never send more than two Sensor Gets.
        if (this->mesh_sensor_rx_.load() == this->poll_sensor_rx_start_)
          this->send_sensor_get_();
        break;
      default:
        break;
    }
    if (advance) {
      this->poll_stage_++;
      if (this->poll_stage_ > 7)
        this->poll_stage_ = 0;
      else
        // The global access-operation slot is released by the response or
        // timeout callback. The short settle delay avoids needless idle time
        // while still guaranteeing that no acknowledged messages overlap.
        this->poll_stage_at_ = now + 250;
    }
  }
}

void NightmatiqMesh::pause_ble_for_cloud_() {
  if (this->mesh_mode_enabled_)
    return;
  this->cloud_api_shutdown_pending_.store(true);
}

void NightmatiqMesh::resume_ble_after_cloud_() {
  if (!this->cloud_ble_resume_pending_.load() || this->mesh_mode_enabled_)
    return;

  if (esp32_ble::global_ble != nullptr && !esp32_ble::global_ble->is_active()) {
    esp32_ble::global_ble->enable();
    return;
  }

  // This standalone gateway has no Bluetooth Proxy role. Leave the setup
  // scanner idle until the next explicit identity scan or Mesh-mode reboot.
  this->cloud_ble_resume_pending_.store(false);
}

}  // namespace nightmatiq_mesh
}  // namespace esphome
