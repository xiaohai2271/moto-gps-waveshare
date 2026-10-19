#pragma once

// Navigation-page layout persistence (docs/NAV_LAYOUT_EDITOR_PLAN.md §6.5).
//
// The phone is the single editor of the navigation layout, but the device keeps
// the last accepted one in NVS so a reboot or a fresh connection shows the same
// page without waiting for a push. One record holds the complete layout plus
// its revision, guarded by a CRC32 and cross-checked against a separate
// revision key: a torn, truncated or stale record is rejected and the caller
// falls back to its own default layout.
//
// The store deliberately speaks the C ABI type of shared/nav_ui
// (moto_ui_layout_t). PhoneNavBridge owns the moto::ble::UiLayout ->
// moto_ui_layout_t mapping (plan §6.2), so this module never depends on the
// BLE protocol headers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "moto_nav_ui.h"

namespace moto_ui_layout_store {

// Scratch capacity for the CRC prefix. The header is a handful of bytes, so a
// generous fixed bound keeps record_crc() free of variable-length arrays.
inline constexpr std::size_t kStoredHeaderBytes = 64;

// NVS namespace and keys (§6.5).
inline constexpr char kNamespace[] = "ui_cfg";
inline constexpr char kLayoutKey[] = "layout";
inline constexpr char kRevisionKey[] = "rev";

// 'M' 'U' 'I' '1' as stored little-endian; the trailing digit is the layout
// schema version, so a future incompatible record is rejected instead of
// interpreted with the old field meanings.
inline constexpr std::uint32_t kStoredMagic = 0x3149554DU;

// One flash record. The CRC covers every byte before crc32, so a partial write
// or a flash bit flip can never be mistaken for a valid layout.
struct StoredLayout {
  std::uint32_t magic = 0;
  std::uint16_t revision = 0;
  std::uint16_t length = 0;
  moto_ui_layout_t layout{};
  std::uint32_t crc32 = 0;
};

static_assert(std::is_trivially_copyable_v<StoredLayout>,
              "the stored layout must stay a plain byte record");

// CRC-32 (IEEE 802.3), the same polynomial ESP-IDF's esp_crc32_le uses, so a
// record written by either implementation stays interchangeable. Keeping one
// shared implementation also lets the host tests exercise the exact record
// layout the firmware writes.
inline std::uint32_t crc32_le(std::uint32_t crc, const std::uint8_t* data,
                              std::size_t length) {
  crc = ~crc;
  for (std::size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(0u - (crc & 1U)) & 0xEDB88320U;
      crc = (crc >> 1U) ^ mask;
    }
  }
  return ~crc;
}

inline std::size_t stored_crc_offset() {
  return offsetof(StoredLayout, crc32);
}

inline std::uint32_t record_crc(const StoredLayout& record) {
  // A fixed-capacity scratch buffer keeps this valid C++ on MSVC too, which
  // rejects the variable-length array spelling.
  std::uint8_t bytes[kStoredHeaderBytes]{};
  const std::size_t length = std::min(sizeof(bytes), stored_crc_offset());
  std::memcpy(bytes, &record, length);
  return crc32_le(0, bytes, length);
}

// Structural integrity only: magic, payload size and CRC. Semantic validation
// lives in valid_layout() so a record from a newer/older schema with a valid
// CRC is still rejected on its contents.
inline bool check_record(const StoredLayout& record) {
  return record.magic == kStoredMagic &&
         record.length == static_cast<std::uint16_t>(sizeof(moto_ui_layout_t)) &&
         record_crc(record) == record.crc32;
}

// Semantic bounds, mirroring shared/ble_protocol's UiLayout validation and
// docs §5.3. A record that fails here is treated as absent (default layout)
// rather than being clamped into something the phone never asked for.
inline bool valid_layout(const moto_ui_layout_t& layout) {
  if (layout.north_up > 1 || layout.music_page_enabled > 1 ||
      layout.map_visible > 1) {
    return false;
  }
  if (layout.theme_id > 3) {
    return false;
  }
  if (layout.map_height_px < MOTO_UI_LAYOUT_MIN_MAP_HEIGHT ||
      layout.map_height_px > MOTO_UI_LAYOUT_MAX_MAP_HEIGHT ||
      (layout.map_height_px & 1U) != 0U) {
    return false;
  }
  if (layout.map_opacity > 100 || layout.route_opacity > 100 ||
      layout.street_opacity > 100 || layout.building_opacity > 100) {
    return false;
  }
  if (layout.route_width_px == 0 || layout.route_width_px > 16 ||
      layout.street_width_px == 0 || layout.street_width_px > 12) {
    return false;
  }
  if (layout.zoom_percent < 50 || layout.zoom_percent > 200) {
    return false;
  }
  if ((layout.layer_mask & ~0x07U) != 0U) {
    return false;
  }
  if (layout.component_count > MOTO_UI_LAYOUT_COMPONENT_COUNT) {
    return false;
  }
  std::uint16_t seen_ids = 0;
  for (std::uint8_t index = 0; index < layout.component_count; ++index) {
    const moto_ui_component_layout_t& component = layout.components[index];
    if (component.id < 1 ||
        component.id > MOTO_UI_LAYOUT_COMPONENT_COUNT) {
      return false;
    }
    // cflags only carries the enabled bit; anything else is a protocol
    // violation, so the whole layout is rejected.
    if (component.enabled > 1) {
      return false;
    }
    if (component.size > MOTO_UI_SIZE_L || component.opacity > 100) {
      return false;
    }
    const std::uint16_t bit =
        static_cast<std::uint16_t>(1U << (component.id - 1U));
    if ((seen_ids & bit) != 0U) {
      return false;
    }
    seen_ids = static_cast<std::uint16_t>(seen_ids | bit);
    const bool is_arc =
        component.id == MOTO_UI_COMPONENT_PROGRESS_ARC ||
        component.id == MOTO_UI_COMPONENT_SPEED_ARC;
    if (is_arc) {
      if (component.x != 0 || component.y != 0) {
        return false;
      }
      if (component.anchor > MOTO_UI_ARC_LEFT) {
        return false;
      }
      if (component.span_deg != 90 && component.span_deg != 120 &&
          component.span_deg != 180) {
        return false;
      }
    } else {
      if (component.anchor != 0 || component.span_deg != 0) {
        return false;
      }
      if (component.x < 0 || component.y < 0 ||
          component.x >= MOTO_UI_CANVAS_WIDTH ||
          component.y >= MOTO_UI_CANVAS_HEIGHT) {
        return false;
      }
    }
  }
  return true;
}

// --- flash I/O -------------------------------------------------------------
// Firmware: NVS, implemented in ui_layout_store.cpp. Host/test builds have no
// NVS, so an in-memory record stands in for it. The shared logic below is
// identical on both, which keeps the bridge's persistence behaviour testable
// without an ESP-IDF dependency in the native test target.

#ifdef ESP_PLATFORM

bool flash_prepare();
bool flash_read(StoredLayout* out);
bool flash_write(const StoredLayout& record);
bool flash_erase();

#else  // ESP_PLATFORM

inline bool& host_record_ready() {
  static bool ready = false;
  return ready;
}

inline StoredLayout& host_record() {
  static StoredLayout record{};
  return record;
}

inline bool flash_prepare() {
  host_record_ready() = true;
  return true;
}

inline bool flash_read(StoredLayout* out) {
  if (out == nullptr || !host_record_ready()) {
    return false;
  }
  const StoredLayout& record = host_record();
  if (!check_record(record) || !valid_layout(record.layout)) {
    return false;
  }
  *out = record;
  return true;
}

inline bool flash_write(const StoredLayout& record) {
  host_record() = record;
  host_record_ready() = true;
  return true;
}

inline bool flash_erase() {
  host_record() = StoredLayout{};
  host_record_ready() = true;
  return true;
}

#endif  // ESP_PLATFORM

// --- shared store logic ----------------------------------------------------

// Makes the flash namespace usable and reports whether it currently holds a
// valid layout. Cheap enough to call once at startup.
inline bool init(void) {
  if (!flash_prepare()) {
    return false;
  }
  StoredLayout record{};
  return flash_read(&record);
}

inline bool load(moto_ui_layout_t* out, std::uint16_t* revision) {
  if (out == nullptr) {
    return false;
  }
  StoredLayout record{};
  if (!flash_read(&record)) {
    return false;
  }
  *out = record.layout;
  if (revision != nullptr) {
    *revision = record.revision;
  }
  return true;
}

// Writes the layout and its revision. The caller owns the debounce (plan
// §5.6): PhoneNavBridge only calls this once a pushed layout has settled, so a
// drag burst costs one flash write instead of one per frame.
inline bool save(const moto_ui_layout_t* layout, std::uint16_t revision) {
  if (layout == nullptr || !valid_layout(*layout)) {
    return false;
  }
  StoredLayout record{};
  record.magic = kStoredMagic;
  record.revision = revision;
  record.length = static_cast<std::uint16_t>(sizeof(moto_ui_layout_t));
  record.layout = *layout;
  record.crc32 = record_crc(record);
  return flash_write(record);
}

// Factory reset: drop the persisted layout so the next boot shows the default.
inline void factory_reset(void) { flash_erase(); }

// Whether the currently persisted record is valid. Reported to the phone as
// UiLayoutState's nvs_saved flag so it can tell a durable layout from one that
// only lives in this power cycle.
inline bool saved(void) {
  StoredLayout record{};
  return flash_read(&record);
}

}  // namespace moto_ui_layout_store
