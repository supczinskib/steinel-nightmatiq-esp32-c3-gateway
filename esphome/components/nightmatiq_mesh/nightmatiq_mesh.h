#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esphome/ota/ota_esphome.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"
#include "esp_ble_mesh_sensor_model_api.h"
#include "esp_ble_mesh_time_scene_model_api.h"
#include "esp_http_client.h"

namespace esphome {
namespace nightmatiq_mesh {

class NightmatiqMesh final : public PollingComponent, public AsyncWebHandler {
 public:
  NightmatiqMesh(web_server_base::WebServerBase *base, ESPHomeOTAComponent *ota)
      : base_(base), ota_(ota) {}

  void set_lux_sensor(sensor::Sensor *value) { this->lux_sensor_ = value; }
  void set_rssi_sensor(sensor::Sensor *value) { this->rssi_sensor_ = value; }
  void set_threshold_number(number::Number *value) { this->threshold_number_ = value; }
  void set_mode_select(select::Select *value) { this->mode_select_ = value; }
  void set_ready_binary_sensor(binary_sensor::BinarySensor *value) { this->ready_binary_sensor_ = value; }
  void set_actual_output_binary_sensor(binary_sensor::BinarySensor *value) {
    this->actual_output_binary_sensor_ = value;
  }
  void set_status_text_sensor(text_sensor::TextSensor *value) { this->status_text_sensor_ = value; }
  void set_firmware_version_text_sensor(text_sensor::TextSensor *value) {
    this->firmware_version_text_sensor_ = value;
  }
  void set_hardware_version_sensor(sensor::Sensor *value) { this->hardware_version_sensor_ = value; }
  void set_manufacturer_text_sensor(text_sensor::TextSensor *value) {
    this->manufacturer_text_sensor_ = value;
  }
  void set_company_id_text_sensor(text_sensor::TextSensor *value) {
    this->company_id_text_sensor_ = value;
  }
  void set_product_id_text_sensor(text_sensor::TextSensor *value) {
    this->product_id_text_sensor_ = value;
  }
  void set_web_credentials(const std::string &username, const std::string &password) {
    this->web_username_ = username;
    this->web_password_ = password;
  }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  void gap_scan_event_handler(const esp32_ble::BLEScanResult &scan_result);

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }

  void set_threshold(float lux);
  void set_mode(const std::string &mode);
  void request_refresh();
  bool mesh_mode_enabled() const { return this->mesh_mode_enabled_; }

  static void provisioning_callback(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param);
  static void config_callback(esp_ble_mesh_cfg_client_cb_event_t event,
                              esp_ble_mesh_cfg_client_cb_param_t *param);
  static void generic_callback(esp_ble_mesh_generic_client_cb_event_t event,
                               esp_ble_mesh_generic_client_cb_param_t *param);
  static void sensor_callback(esp_ble_mesh_sensor_client_cb_event_t event,
                              esp_ble_mesh_sensor_client_cb_param_t *param);
  static void light_callback(esp_ble_mesh_light_client_cb_event_t event,
                             esp_ble_mesh_light_client_cb_param_t *param);
  static void scene_callback(esp_ble_mesh_time_scene_client_cb_event_t event,
                             esp_ble_mesh_time_scene_client_cb_param_t *param);
 protected:
  static constexpr uint32_t CONFIG_MAGIC = 0x4E4D5131U;  // "NMQ1"
  static constexpr uint16_t CONFIG_VERSION = 2;
  static constexpr uint32_t DEVICE_KEY_MAGIC = 0x4E4D514BU;  // "NMQK"
  static constexpr uint16_t DEVICE_KEY_VERSION = 1;
  static constexpr uint32_t RETIRED_ADDRESS_MAGIC = 0x4E4D5141U;  // "NMQA"
  static constexpr uint16_t RETIRED_ADDRESS_VERSION = 1;
  static constexpr uint32_t ADDRESS_POLICY_MAGIC = 0x4E4D5150U;  // "NMQP"
  static constexpr uint16_t ADDRESS_POLICY_VERSION = 1;
  static constexpr uint16_t ADDRESS_POOL_TARGET_SIZE = 2048;
  static constexpr uint16_t AUTO_ADDRESS_ROTATION_LIMIT = 16;
  static constexpr uint32_t AUTO_ADDRESS_RECOVERY_DELAY_MS = 60000;
  static constexpr uint32_t AUTO_ADDRESS_MIN_ACCEPTED_TX = 10;
  static constexpr uint32_t AUTO_ADDRESS_MIN_TIMEOUTS = 10;
  static constexpr uint32_t ADDRESS_CONFIRMATION_MAGIC = 0x4E4D5143U;  // "NMQC"
  static constexpr uint16_t ADDRESS_CONFIRMATION_VERSION = 1;
  static constexpr uint32_t IV_CACHE_MAGIC = 0x4E4D5149U;  // "NMQI"
  static constexpr uint16_t IV_CACHE_VERSION = 1;
  static constexpr uint32_t ADVERTISED_IDENTITY_MAGIC = 0x4E4D5156U;  // "NMQV"
  static constexpr uint16_t ADVERTISED_IDENTITY_VERSION = 2;
  static constexpr uint32_t ADMIN_CREDENTIALS_MAGIC = 0x4E4D5157U;  // "NMQW"
  static constexpr uint16_t ADMIN_CREDENTIALS_VERSION = 1;
  static constexpr size_t ADMIN_PASSWORD_MIN_LENGTH = 8;
  static constexpr size_t ADMIN_PASSWORD_MAX_LENGTH = 63;
  static constexpr uint32_t AUTO_UPDATE_MAGIC = 0x4E4D5155U;  // "NMQU"
  static constexpr uint16_t AUTO_UPDATE_VERSION = 1;
  static constexpr size_t AUTO_UPDATE_VERSION_MAX_LENGTH = 23;
  static constexpr size_t AUTO_UPDATE_URL_MAX_LENGTH = 255;
  static constexpr uint16_t STEINEL_COMPANY_ID = 0x0563;
  static constexpr uint16_t NIGHTMATIQ_PRODUCT_ID = 0x1DCE;
  static constexpr uint16_t FLAG_ENABLED = 0x0001;
  static constexpr uint16_t FLAG_REMOVE_PENDING = 0x0002;
  // Set only after a Secure Network Beacon changes the IV Index or an
  // authenticated Access response proves that the stored value is usable.
  static constexpr uint16_t FLAG_IV_INDEX_CONFIRMED = 0x0004;
  static constexpr size_t MAX_DISCOVERY_RESPONSE_BYTES = 32 * 1024;
  static constexpr uint32_t ACTUAL_OUTPUT_STALE_MS = 5UL * 60UL * 1000UL;
  // A mode SET is intentionally unacknowledged for immediate lamp control.
  // Confirm its physical result several times before returning to the normal
  // Retry within the 30-second polling interval after an unanswered GET.
  static constexpr uint8_t OUTPUT_CONFIRMATION_MAX_ATTEMPTS = 5;

  struct StoredConfig {
    uint32_t magic{CONFIG_MAGIC};
    uint16_t version{CONFIG_VERSION};
    uint16_t net_key_index{0};
    uint16_t app_key_index{0};
    uint16_t local_address{0};
    uint16_t onoff_address{0};
    uint16_t lc_address{0};
    uint16_t sensor_address{0};
    uint16_t scene_number{0};
    uint16_t flags{0};
    uint32_t iv_index{0};
    std::array<uint8_t, 16> net_key{};
    std::array<uint8_t, 16> app_key{};
    std::array<uint8_t, 16> mesh_uuid{};
    char network_name[48]{};
    char node_name[48]{};
  };

  struct StoredDeviceKey {
    uint32_t magic{DEVICE_KEY_MAGIC};
    uint16_t version{DEVICE_KEY_VERSION};
    std::array<uint8_t, 16> key{};
  };

  struct StoredRetiredAddress {
    uint32_t magic{RETIRED_ADDRESS_MAGIC};
    uint16_t version{RETIRED_ADDRESS_VERSION};
    uint16_t address{0};
  };

  struct StoredAddressPolicy {
    uint32_t magic{ADDRESS_POLICY_MAGIC};
    uint16_t version{ADDRESS_POLICY_VERSION};
    uint16_t pool_low{0};
    uint16_t pool_high{0};
    uint16_t initial_address{0};
    uint16_t current_address{0};
    uint16_t automatic_rotations{0};
    // Retained to preserve the version-1 NVS record layout. Manual rotation
    // is no longer exposed by the standalone gateway.
    uint16_t reserved{0};
    uint32_t installation_nonce{0};
    std::array<uint8_t, 16> mesh_uuid{};
  };

  struct StoredAddressConfirmation {
    uint32_t magic{ADDRESS_CONFIRMATION_MAGIC};
    uint16_t version{ADDRESS_CONFIRMATION_VERSION};
    uint16_t address{0};
    uint32_t installation_nonce{0};
    std::array<uint8_t, 16> mesh_uuid{};
  };

  struct StoredIvCache {
    uint32_t magic{IV_CACHE_MAGIC};
    uint16_t version{IV_CACHE_VERSION};
    uint16_t reserved{0};
    uint32_t iv_index{0};
    std::array<uint8_t, 16> mesh_uuid{};
  };

  struct StoredAdminCredentials {
    uint32_t magic{ADMIN_CREDENTIALS_MAGIC};
    uint16_t version{ADMIN_CREDENTIALS_VERSION};
    char password[ADMIN_PASSWORD_MAX_LENGTH + 1]{};
  };

  struct StoredAutoUpdate {
    uint32_t magic{AUTO_UPDATE_MAGIC};
    uint16_t version{AUTO_UPDATE_VERSION};
    uint16_t reserved{0};
    uint32_t image_size{0};
    char target_version[AUTO_UPDATE_VERSION_MAX_LENGTH + 1]{};
    char url[AUTO_UPDATE_URL_MAX_LENGTH + 1]{};
    std::array<uint8_t, 32> sha256{};
  };

  struct StoredAdvertisedIdentity {
    uint32_t magic{ADVERTISED_IDENTITY_MAGIC};
    uint16_t version{ADVERTISED_IDENTITY_VERSION};
    uint16_t product_id{0};
    uint8_t firmware_major{0};
    uint8_t firmware_minor{0};
    uint8_t firmware_patch{0};
    uint8_t bootloader_version{0};
    uint8_t hardware_version{0};
    uint8_t reserved{0};
    uint16_t firmware_hash{0};
    uint16_t composition_version_id{0};
  };

  struct NetworkChoice {
    std::string id;
    std::string name;
    std::string last_update;
    uint16_t nodes{0};
  };

  enum class AccessOperation : uint8_t {
    NONE,
    SENSOR_GET,
    REVISION_CATALOG_GET,
    THRESHOLD_GET,
    LC_MODE_GET,
    ONOFF_GET,
    SCENE_GET,
    THRESHOLD_SET,
    LC_MODE_SET,
    ONOFF_SET,
    SCENE_RECALL,
  };
  enum class ControlKind : uint8_t { NONE, MODE, THRESHOLD };
  enum class ControlStep : uint8_t {
    IDLE,
    MODE_SEND_LC,
    MODE_SEND_ACTION,
    MODE_REPEAT_LC,
    MODE_REPEAT_ACTION,
    THRESHOLD_SEND_SET,
    THRESHOLD_WAIT_SET,
  };
  enum class CloudJob : uint8_t { DISCOVER, INSTALL };
  struct CloudBody;
  struct CloudTaskArgs;
  struct AutoUpdateContext;

  bool initialize_bluetooth_();
  bool initialize_mesh_();
  bool deinitialize_mesh_(bool erase_flash);
  bool restore_target_node_();
  void advance_mesh_start_();
  void begin_identity_scan_();
  void advance_identity_scan_();
  bool capture_advertised_identity_(const uint8_t *data, size_t length, int16_t rssi);
  void persist_pending_advertised_identity_();
  void advance_mesh_remove_();
  void advance_factory_reset_();
  void monitor_iv_index_();
  bool load_config_();
  bool load_device_key_();
  bool load_admin_credentials_();
  bool save_admin_password_(const std::string &password);
  void apply_admin_credentials_();
  static bool valid_admin_password_(const std::string &password);
  bool load_auto_update_();
  bool save_auto_update_(const StoredAutoUpdate &update);
  bool clear_auto_update_();
  void advance_auto_update_();
  void fail_auto_update_(const std::string &error);
  static void auto_update_task_(void *parameter);
  static esp_err_t auto_update_http_event_(esp_http_client_event_t *event);
  void load_retired_address_();
  void retire_local_address_();
  bool load_address_policy_();
  bool save_address_policy_(const StoredAddressPolicy &policy);
  bool select_next_local_address_(uint16_t &address) const;
  bool rotate_local_address_(std::string &error);
  void advance_address_recovery_(uint32_t now);
  bool load_address_confirmation_();
  bool current_address_confirmed_() const;
  void persist_address_confirmation_();
  bool load_cached_iv_index_(const std::array<uint8_t, 16> &mesh_uuid, uint32_t &iv_index);
  void remember_iv_index_(const StoredConfig &config);
  bool load_advertised_identity_();
  bool save_advertised_identity_(const StoredAdvertisedIdentity &identity);
  void clear_advertised_identity_();
  bool advertised_identity_current_() const;
  bool resolve_firmware_version_(uint8_t &major, uint8_t &minor, uint8_t &patch) const;
  bool parse_scan_result_(const esp32_ble::BLEScanResult &scan_result);
  void finish_identity_scan_();
  bool save_config_(const StoredConfig &config);
  bool save_device_key_(const std::array<uint8_t, 16> &device_key);
  bool save_enabled_(bool enabled);
  void clear_config_();
  bool parse_backup_(const CloudBody &body, uint32_t requested_iv_index,
                     uint16_t requested_node_address, StoredConfig &config,
                     std::array<uint8_t, 16> &device_key,
                     StoredAddressPolicy &address_policy, std::string &error);
  bool cloud_get_(const std::string &path, const std::string &email, const std::string &password,
                  bool use_ota_workspace, CloudBody &body, int &http_status, std::string &error);
  bool discover_networks_(const std::string &email, const std::string &password, std::string &error);
  bool install_network_(const std::string &email, const std::string &password,
                        const std::string &network_id, uint32_t iv_index,
                        uint16_t node_address, std::string &error);
  bool start_cloud_job_(CloudJob job, const std::string &email, const std::string &password,
                        const std::string &network_id = {}, uint32_t iv_index = 0,
                        uint16_t node_address = 0);
  void pause_ble_for_cloud_();
  void advance_cloud_job_();
  void resume_ble_after_cloud_();
  void schedule_cloud_session_reboot_(uint32_t delay_ms);
  static void cloud_task_(void *parameter);
  static esp_err_t cloud_http_event_(esp_http_client_event_t *event);

  bool authenticate_(AsyncWebServerRequest *request) const;
  void handle_index_(AsyncWebServerRequest *request);
  void handle_status_(AsyncWebServerRequest *request);
  void handle_discover_(AsyncWebServerRequest *request);
  void handle_install_(AsyncWebServerRequest *request);
  void handle_enable_(AsyncWebServerRequest *request);
  void handle_disable_(AsyncWebServerRequest *request);
  void handle_remove_(AsyncWebServerRequest *request);
  void handle_factory_reset_(AsyncWebServerRequest *request);
  void handle_mode_(AsyncWebServerRequest *request);
  void handle_threshold_(AsyncWebServerRequest *request);
  void handle_refresh_(AsyncWebServerRequest *request);
  void handle_password_(AsyncWebServerRequest *request);
  void handle_auto_update_(AsyncWebServerRequest *request);
  static void send_json_(AsyncWebServerRequest *request, int code, const std::string &body);
  static bool parse_u32_(const std::string &value, uint32_t minimum, uint32_t maximum, uint32_t &output);
  static bool parse_hex_u16_(const std::string &value, uint16_t &output);
  static bool is_safe_uuid_(const std::string &value);
  static bool parse_version_(const std::string &value, std::array<uint16_t, 3> &version);
  static bool parse_sha256_(const std::string &value, std::array<uint8_t, 32> &digest);
  void set_status_(const std::string &status, bool publish = true);
  bool set_common_(esp_ble_mesh_client_common_param_t &common, esp_ble_mesh_model_t *model,
                   uint32_t opcode, uint16_t destination);
  bool record_send_result_(esp_err_t result);
  bool begin_access_operation_(AccessOperation operation, uint32_t opcode);
  bool record_access_send_result_(AccessOperation operation, uint32_t opcode, esp_err_t result);
  bool complete_access_operation_(uint32_t opcode, bool success);
  void expire_access_operation_(uint32_t now);
  void record_mesh_rssi_(const esp_ble_mesh_msg_ctx_t &context);
  bool control_request_pending_() const;
  void start_next_control_(uint32_t now);
  void advance_control_(uint32_t now);
  void retry_or_finish_control_(bool success, uint32_t now);
  void finish_control_(bool success, uint32_t now);
  void bind_model_(uint16_t model_id);
  void keys_bound_();
  void mark_ready_();
  void publish_pending_();
  void publish_mode_from_observed_();
  void record_actual_output_(bool on);
  void force_actual_output_unavailable_();

  bool send_sensor_get_();
  bool send_device_revision_catalog_get_();
  bool send_threshold_get_();
  bool send_lc_mode_get_();
  bool send_onoff_get_();
  bool send_scene_get_();
  bool send_composition_get_();
  bool send_threshold_set_(uint32_t centilux);
  bool send_lc_mode_set_(bool enabled);
  bool send_onoff_set_(bool on);
  bool send_scene_recall_();
  uint8_t next_tid_();

  static NightmatiqMesh *instance_;

  web_server_base::WebServerBase *base_;
  ESPHomeOTAComponent *ota_;
  ESPPreferenceObject config_preference_;
  ESPPreferenceObject device_key_preference_;
  ESPPreferenceObject retired_address_preference_;
  ESPPreferenceObject address_policy_preference_;
  ESPPreferenceObject address_confirmation_preference_;
  ESPPreferenceObject iv_cache_preference_;
  ESPPreferenceObject advertised_identity_preference_;
  ESPPreferenceObject admin_credentials_preference_;
  ESPPreferenceObject auto_update_preference_;
  StoredConfig config_{};
  std::array<uint8_t, 16> device_key_{};
  bool device_key_valid_{false};
  uint16_t retired_local_address_{0};
  StoredAddressPolicy address_policy_{};
  bool address_policy_valid_{false};
  StoredAddressConfirmation address_confirmation_{};
  bool address_confirmation_valid_{false};
  bool address_confirmation_save_attempted_this_boot_{false};
  bool configured_{false};
  bool mesh_mode_enabled_{false};
  bool mesh_started_{false};
  bool mesh_start_pending_{false};
  std::atomic<bool> mesh_remove_pending_{false};
  uint32_t mesh_remove_not_before_{0};
  std::atomic<bool> factory_reset_pending_{false};
  uint32_t factory_reset_at_{0};
  uint32_t mesh_start_not_before_{0};
  uint32_t mesh_start_deadline_{0};
  std::atomic<bool> identity_scan_pending_{false};
  std::atomic<bool> identity_found_this_boot_{false};
  bool identity_scan_started_{false};
  enum class IdentityScanPhase : uint8_t {
    WAIT_FOR_BLE,
    WAIT_FOR_RANDOM_ADDRESS,
    WAIT_FOR_SCAN_PARAMS,
    WAIT_FOR_SCAN_START,
    RUNNING,
    WAIT_FOR_SCAN_STOP,
  };
  IdentityScanPhase identity_scan_phase_{IdentityScanPhase::WAIT_FOR_BLE};
  esp_ble_scan_params_t identity_scan_params_{};
  std::atomic<bool> identity_scan_params_ready_{false};
  std::atomic<bool> identity_scan_start_ready_{false};
  std::atomic<bool> identity_scan_stop_ready_{false};
  std::atomic<bool> identity_scan_command_error_{false};
  uint32_t identity_scan_action_at_{0};
  uint32_t identity_scan_deadline_{0};
  std::atomic<bool> advertised_identity_save_pending_{false};
  uint32_t iv_index_check_at_{0};
  bool keys_bound_pending_{false};
  uint32_t keys_bound_at_{0};
  uint8_t tid_{0};

  sensor::Sensor *lux_sensor_{nullptr};
  sensor::Sensor *rssi_sensor_{nullptr};
  number::Number *threshold_number_{nullptr};
  select::Select *mode_select_{nullptr};
  binary_sensor::BinarySensor *ready_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *actual_output_binary_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *firmware_version_text_sensor_{nullptr};
  sensor::Sensor *hardware_version_sensor_{nullptr};
  text_sensor::TextSensor *manufacturer_text_sensor_{nullptr};
  text_sensor::TextSensor *company_id_text_sensor_{nullptr};
  text_sensor::TextSensor *product_id_text_sensor_{nullptr};

  std::string web_username_;
  std::string web_password_;
  bool using_factory_admin_password_{true};
  StoredAutoUpdate auto_update_{};
  bool auto_update_mode_{false};
  bool auto_update_api_shutdown_started_{false};
  bool auto_update_ble_disable_started_{false};
  uint32_t auto_update_stage_deadline_{0};
  std::atomic<bool> auto_update_running_{false};
  std::atomic<uint8_t> auto_update_progress_{0};
  std::mutex state_mutex_;
  std::string status_{"Configuration required"};
  std::vector<NetworkChoice> networks_;
  std::atomic<bool> cloud_busy_{false};
  std::atomic<CloudTaskArgs *> cloud_pending_args_{nullptr};
  std::atomic<bool> cloud_api_shutdown_pending_{false};
  std::atomic<bool> cloud_api_shutdown_started_{false};
  std::atomic<uint32_t> cloud_api_shutdown_deadline_{0};
  std::atomic<bool> cloud_ble_pause_pending_{false};
  std::atomic<bool> cloud_ble_resume_pending_{false};
  std::atomic<uint32_t> cloud_ble_pause_deadline_{0};
  std::atomic<bool> cloud_session_reboot_pending_{false};
  std::atomic<uint32_t> cloud_session_reboot_at_{0};
  std::atomic<uint32_t> cloud_free_after_ble_{0};
  std::atomic<uint32_t> cloud_largest_after_ble_{0};
  std::atomic<uint32_t> cloud_response_bytes_{0};
  std::atomic<bool> reboot_pending_{false};
  uint32_t reboot_at_{0};

  std::atomic<bool> mesh_ready_{false};
  uint32_t mesh_ready_at_{0};
  bool address_recovery_attempted_this_boot_{false};
  std::atomic<uint32_t> live_iv_index_{0};
  std::atomic<bool> live_iv_index_confirmed_{false};
  std::atomic<bool> ready_publish_pending_{false};
  std::atomic<bool> web_refresh_pending_{false};
  std::atomic<bool> status_publish_pending_{false};
  std::atomic<bool> lux_publish_pending_{false};
  std::atomic<uint32_t> pending_lux_centilux_{0};
  std::atomic<bool> threshold_publish_pending_{false};
  std::atomic<bool> threshold_invalidate_pending_{false};
  std::atomic<bool> threshold_received_{false};
  std::atomic<uint32_t> pending_threshold_centilux_{0};
  std::atomic<uint32_t> observed_threshold_centilux_{0};
  std::atomic<uint32_t> threshold_response_sequence_{0};
  std::atomic<int8_t> observed_lc_mode_{-1};
  std::atomic<uint32_t> lc_mode_response_sequence_{0};
  std::atomic<int8_t> observed_onoff_{-1};
  std::atomic<uint32_t> onoff_response_sequence_{0};
  std::atomic<bool> actual_output_publish_pending_{false};
  std::atomic<bool> actual_output_invalidate_pending_{false};
  std::atomic<bool> actual_output_forced_unavailable_{true};
  std::atomic<uint32_t> actual_output_last_response_at_{0};
  bool actual_output_available_{false};
  std::atomic<int8_t> mode_publish_pending_{-1};
  std::atomic<uint32_t> mesh_tx_attempts_{0};
  std::atomic<uint32_t> mesh_tx_accepted_{0};
  std::atomic<uint32_t> mesh_tx_errors_{0};
  std::atomic<int32_t> mesh_last_tx_error_{0};
  std::atomic<uint32_t> mesh_rx_messages_{0};
  std::atomic<uint32_t> mesh_timeouts_{0};
  std::atomic<int16_t> last_mesh_rssi_dbm_{0};
  std::atomic<uint32_t> last_mesh_rssi_at_{0};
  std::atomic<bool> mesh_rssi_received_{false};
  std::atomic<bool> mesh_rssi_publish_pending_{false};
  std::atomic<uint32_t> mesh_generic_rx_{0};
  std::atomic<uint32_t> mesh_sensor_rx_{0};
  std::atomic<uint32_t> mesh_light_rx_{0};
  std::atomic<uint32_t> mesh_scene_rx_{0};
  std::atomic<uint32_t> mesh_generic_timeouts_{0};
  std::atomic<uint32_t> mesh_sensor_timeouts_{0};
  std::atomic<uint32_t> mesh_light_timeouts_{0};
  std::atomic<uint32_t> mesh_scene_timeouts_{0};
  std::atomic<bool> lux_received_{false};
  std::atomic<bool> composition_query_pending_{false};
  std::atomic<bool> composition_query_in_flight_{false};
  std::atomic<uint32_t> composition_query_at_{0};
  std::atomic<uint32_t> composition_query_attempts_{0};
  std::atomic<uint32_t> composition_query_failures_{0};
  std::atomic<uint32_t> composition_query_deadline_{0};
  std::atomic<uint32_t> composition_responses_{0};
  std::atomic<uint32_t> composition_timeouts_{0};
  std::atomic<int32_t> composition_last_event_{-1};
  std::atomic<int32_t> composition_last_error_{0};
  std::atomic<uint32_t> composition_last_opcode_{0};
  std::atomic<bool> composition_received_{false};
  std::atomic<uint16_t> live_company_id_{0};
  std::atomic<uint16_t> live_product_id_{0};
  std::atomic<uint16_t> live_version_id_{0};
  std::atomic<int16_t> live_firmware_revision_{-1};
  std::atomic<int16_t> live_hardware_revision_{-1};
  std::atomic<bool> revision_catalog_in_flight_{false};
  std::atomic<bool> advertised_identity_valid_{false};
  std::atomic<bool> advertised_identity_fresh_{false};
  std::atomic<bool> advertised_identity_publish_pending_{false};
  std::atomic<uint16_t> advertised_product_id_{0};
  std::atomic<uint8_t> advertised_firmware_major_{0};
  std::atomic<uint8_t> advertised_firmware_minor_{0};
  std::atomic<uint8_t> advertised_firmware_patch_{0};
  std::atomic<uint8_t> advertised_bootloader_version_{0};
  std::atomic<uint8_t> advertised_hardware_version_{0};
  std::atomic<uint16_t> advertised_firmware_hash_{0};
  std::atomic<uint16_t> advertised_composition_version_id_{0};
  std::atomic<int16_t> advertised_rssi_{0};

  std::atomic<AccessOperation> access_operation_{AccessOperation::NONE};
  std::atomic<uint32_t> access_opcode_{0};
  std::atomic<uint32_t> access_deadline_{0};
  std::atomic<AccessOperation> access_last_completed_{AccessOperation::NONE};
  std::atomic<bool> access_last_success_{false};

  std::atomic<int8_t> requested_mode_{-1};
  std::atomic<uint32_t> mode_request_sequence_{0};
  std::atomic<bool> mode_override_pending_{false};
  std::atomic<uint32_t> requested_threshold_centilux_{0};
  std::atomic<uint32_t> threshold_request_sequence_{0};
  std::atomic<bool> threshold_override_pending_{false};
  uint32_t handled_mode_request_sequence_{0};
  uint32_t handled_threshold_request_sequence_{0};
  ControlKind control_kind_{ControlKind::NONE};
  ControlStep control_step_{ControlStep::IDLE};
  uint8_t control_attempt_{0};
  uint8_t control_tid_{0};
  uint32_t control_action_at_{0};
  uint32_t control_response_sequence_{0};
  uint32_t control_previous_threshold_centilux_{0};
  std::atomic<uint32_t> mode_confirmation_deadline_{0};

  // ESP-IDF deep-copies the Light LC SET structure but not the property-value
  // bytes referenced by it. Keep both objects alive until the acknowledged
  // transaction completes; a stack buffer here corrupts the written lux value.
  std::array<uint8_t, 3> threshold_set_storage_{};
  net_buf_simple threshold_set_buffer_{};

  uint8_t poll_stage_{0};
  uint32_t poll_stage_at_{0};
  uint32_t poll_sensor_rx_start_{0};
  uint32_t poll_generic_rx_start_{0};
  bool output_confirmation_pending_{false};
  uint8_t output_confirmation_attempts_{0};
  bool initial_poll_started_{false};
};

}  // namespace nightmatiq_mesh
}  // namespace esphome
