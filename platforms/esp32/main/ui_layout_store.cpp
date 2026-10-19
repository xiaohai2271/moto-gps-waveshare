#include "ui_layout_store.h"

#ifdef ESP_PLATFORM

#include <cstdio>

#include "esp_crc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "ui_layout_store";
}  // namespace

namespace moto_ui_layout_store {

bool flash_prepare() {
  // nvs_flash_init() is idempotent: it returns ESP_OK for a partition that is
  // already initialized (ESP-IDF NVSPartitionManager::init_partition). The
  // transport already ran it for its own bonding store, so this must not erase
  // anything; it only guarantees the "ui_cfg" namespace can be opened.
  const esp_err_t result = nvs_flash_init();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "NVS init failed: %s", esp_err_to_name(result));
    return false;
  }
  return true;
}

bool flash_read(StoredLayout* out) {
  if (out == nullptr) {
    return false;
  }
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result != ESP_OK) {
    return false;
  }
  StoredLayout record{};
  std::size_t length = sizeof(record);
  result = nvs_get_blob(handle, kLayoutKey, &record, &length);
  if (result != ESP_OK || length != sizeof(record)) {
    nvs_close(handle);
    return false;
  }
  std::uint16_t revision = 0;
  result = nvs_get_u16(handle, kRevisionKey, &revision);
  nvs_close(handle);
  if (result != ESP_OK) {
    return false;
  }
  if (record.revision != revision) {
    // A torn write can leave the blob and the revision disagreeing. Reject the
    // whole record rather than trusting either half.
    ESP_LOGW(kTag, "layout record revision mismatch: blob=%u rev=%u",
             static_cast<unsigned>(record.revision),
             static_cast<unsigned>(revision));
    return false;
  }
  if (!check_record(record) || !valid_layout(record.layout)) {
    ESP_LOGW(kTag, "stored layout failed validation; using the default layout");
    return false;
  }
  *out = record;
  return true;
}

bool flash_write(const StoredLayout& record) {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "could not open %s: %s", kNamespace,
             esp_err_to_name(result));
    return false;
  }
  // A partial write of the blob must never be able to revive an older record
  // with a newer revision key, so erase the keys before writing the pair.
  result = nvs_erase_key(handle, kLayoutKey);
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    ESP_LOGE(kTag, "could not erase the stored layout: %s",
             esp_err_to_name(result));
    return false;
  }
  result = nvs_erase_key(handle, kRevisionKey);
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    ESP_LOGE(kTag, "could not erase the stored revision: %s",
             esp_err_to_name(result));
    return false;
  }
  result = nvs_set_blob(handle, kLayoutKey, &record, sizeof(record));
  if (result != ESP_OK) {
    nvs_close(handle);
    ESP_LOGE(kTag, "could not write the layout: %s", esp_err_to_name(result));
    return false;
  }
  result = nvs_set_u16(handle, kRevisionKey, record.revision);
  if (result != ESP_OK) {
    nvs_close(handle);
    ESP_LOGE(kTag, "could not write the revision: %s", esp_err_to_name(result));
    return false;
  }
  result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "could not commit the layout: %s", esp_err_to_name(result));
    return false;
  }
  ESP_LOGI(kTag, "layout revision %u persisted",
           static_cast<unsigned>(record.revision));
  return true;
}

bool flash_erase() {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) {
    return false;
  }
  result = nvs_erase_key(handle, kLayoutKey);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    result = ESP_OK;
  }
  if (result == ESP_OK) {
    result = nvs_erase_key(handle, kRevisionKey);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
      result = ESP_OK;
    }
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }
  nvs_close(handle);
  return result == ESP_OK;
}

}  // namespace moto_ui_layout_store

#endif  // ESP_PLATFORM
