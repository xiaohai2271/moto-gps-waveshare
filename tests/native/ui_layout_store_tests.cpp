// Host-side unit tests for the navigation-layout NVS store
// (docs/NAV_LAYOUT_EDITOR_PLAN.md §6.5 and §9.2).
//
// ui_layout_store.h is header-only in host builds: the ESP_PLATFORM branch is
// replaced by an in-memory record behind the same flash_* API, so these tests
// exercise the exact record encoding, CRC and semantic validation the firmware
// relies on, without any ESP-IDF dependency.

#include "ui_layout_store.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

namespace store = moto_ui_layout_store;

int failures = 0;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__                             \
                << " CHECK failed: " #condition << '\n';                 \
      ++failures;                                                          \
    }                                                                      \
  } while (false)

// --- fixtures --------------------------------------------------------------

// Every field sits inside its documented bound, so valid_layout() accepts it.
// Components 1..6 are widgets anchored by their top-left corner; 7 and 8 are
// the two arcs, which carry an anchor and a span instead of coordinates.
moto_ui_layout_t layout_fixture() {
  moto_ui_layout_t layout{};
  layout.north_up = 1;
  layout.music_page_enabled = 1;
  layout.map_visible = 1;
  layout.theme_id = 1;
  layout.map_height_px = MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT;
  layout.map_opacity = 80;
  layout.route_r = 10;
  layout.route_g = 200;
  layout.route_b = 120;
  layout.route_width_px = 6;
  layout.route_opacity = 90;
  layout.street_r = 60;
  layout.street_g = 60;
  layout.street_b = 70;
  layout.street_width_px = 2;
  layout.street_opacity = 40;
  layout.building_opacity = 30;
  layout.layer_mask = 0x07U;
  layout.zoom_percent = 100;
  layout.component_count = 8;

  for (std::uint8_t index = 0; index < 6; ++index) {
    moto_ui_component_layout_t& component = layout.components[index];
    component.id = static_cast<std::uint8_t>(index + 1U);
    component.enabled = (index % 2U == 0U) ? 1U : 0U;
    component.size = static_cast<std::uint8_t>(index % 3U);
    component.opacity = 100;
    component.x = static_cast<std::int16_t>(10 + index * 20);
    component.y = static_cast<std::int16_t>(200 - index * 10);
    component.anchor = 0;
    component.span_deg = 0;
  }

  moto_ui_component_layout_t& progress = layout.components[6];
  progress.id = MOTO_UI_COMPONENT_PROGRESS_ARC;
  progress.enabled = 1;
  progress.size = MOTO_UI_SIZE_L;
  progress.opacity = 90;
  progress.x = 0;
  progress.y = 0;
  progress.anchor = MOTO_UI_ARC_LEFT;
  progress.span_deg = 120;

  moto_ui_component_layout_t& speed = layout.components[7];
  speed.id = MOTO_UI_COMPONENT_SPEED_ARC;
  speed.enabled = 1;
  speed.size = MOTO_UI_SIZE_M;
  speed.opacity = 100;
  speed.x = 0;
  speed.y = 0;
  speed.anchor = MOTO_UI_ARC_RIGHT;
  speed.span_deg = 90;
  return layout;
}

// A layout with exactly one component so a per-component bound can be probed
// without also tripping the duplicate-id check. Arc ids get the arc fields a
// valid layout requires; everything else gets a valid widget corner.
moto_ui_layout_t single_component_layout(std::uint8_t id) {
  moto_ui_layout_t layout = layout_fixture();
  layout.component_count = 1;

  moto_ui_component_layout_t component{};
  component.id = id;
  component.enabled = 1;
  component.size = MOTO_UI_SIZE_M;
  component.opacity = 100;
  if (id == MOTO_UI_COMPONENT_PROGRESS_ARC ||
      id == MOTO_UI_COMPONENT_SPEED_ARC) {
    component.x = 0;
    component.y = 0;
    component.anchor = MOTO_UI_ARC_TOP;
    component.span_deg = 180;
  } else {
    component.x = 100;
    component.y = 300;
    component.anchor = 0;
    component.span_deg = 0;
  }
  layout.components[0] = component;
  return layout;
}

// --- in-memory flash helpers ----------------------------------------------

// Puts the host flash stand-in back into the virgin-device state.
void reset_flash() {
  store::host_record() = store::StoredLayout{};
  store::host_record_ready() = false;
}

// Simulates flash content that save() did not produce (corruption, a torn
// write, or a record written by a different firmware build).
void place_record(const store::StoredLayout& record) {
  store::host_record() = record;
  store::host_record_ready() = true;
}

// The record save() would write for the reference layout.
store::StoredLayout valid_record(std::uint16_t revision = 7) {
  store::StoredLayout record{};
  record.magic = store::kStoredMagic;
  record.revision = revision;
  record.length = static_cast<std::uint16_t>(sizeof(moto_ui_layout_t));
  record.layout = layout_fixture();
  record.crc32 = store::record_crc(record);
  return record;
}

// --- CRC contract ----------------------------------------------------------

void test_crc32_contract() {
  // The store must stay bit-compatible with ESP-IDF's esp_crc32_le, so pin the
  // standard CRC-32 (IEEE 802.3) check value.
  const char* digits = "123456789";
  CHECK(store::crc32_le(0, reinterpret_cast<const std::uint8_t*>(digits), 9) ==
        0xCBF43926U);
  CHECK(store::crc32_le(0, nullptr, 0) == 0U);

  CHECK(store::kStoredMagic == 0x3149554DU);
  CHECK(store::stored_crc_offset() == offsetof(store::StoredLayout, crc32));

  const store::StoredLayout record = valid_record();
  CHECK(store::check_record(record));
  CHECK(static_cast<std::size_t>(record.length) ==
        sizeof(moto_ui_layout_t));
  CHECK(static_cast<std::size_t>(record.length) ==
        sizeof(store::StoredLayout::layout));

  // whole-record check: a changed field invalidates it until the CRC is
  // recomputed.
  store::StoredLayout damaged = record;
  damaged.layout.map_opacity ^= 0x01U;
  CHECK(!store::check_record(damaged));
  damaged.crc32 = store::record_crc(damaged);
  CHECK(store::check_record(damaged));
}

// --- persistence behaviour -------------------------------------------------

void test_fresh_device_has_no_saved_layout() {
  reset_flash();
  CHECK(!store::init());
  CHECK(!store::saved());

  moto_ui_layout_t layout = layout_fixture();
  std::uint16_t revision = 0xFFFFU;
  CHECK(!store::load(&layout, &revision));
  // A failed load must not touch the caller's output.
  CHECK(revision == 0xFFFFU);

  // load() also refuses a null destination without reading flash.
  CHECK(!store::load(nullptr, nullptr));
}

void test_save_and_load_round_trip() {
  reset_flash();
  const moto_ui_layout_t expected = layout_fixture();
  CHECK(store::save(&expected, 4'321));
  CHECK(store::saved());
  CHECK(store::init());

  moto_ui_layout_t loaded{};
  std::uint16_t revision = 0;
  CHECK(store::load(&loaded, &revision));
  CHECK(revision == 4'321);
  CHECK(std::memcmp(&loaded, &expected, sizeof(expected)) == 0);

  // The revision out-parameter is optional.
  moto_ui_layout_t without_revision{};
  CHECK(store::load(&without_revision, nullptr));
  CHECK(std::memcmp(&without_revision, &expected, sizeof(expected)) == 0);

  // A later save replaces the complete record, revision included.
  moto_ui_layout_t next = layout_fixture();
  next.map_height_px = 300;
  next.theme_id = 2;
  CHECK(store::save(&next, 4'322));
  CHECK(store::load(&loaded, &revision));
  CHECK(revision == 4'322);
  CHECK(loaded.map_height_px == 300);
  CHECK(loaded.theme_id == 2);
  CHECK(std::memcmp(&loaded, &next, sizeof(next)) == 0);
}

void test_persisted_record_survives_restart() {
  reset_flash();
  const moto_ui_layout_t expected = layout_fixture();
  CHECK(store::save(&expected, 77));
  const store::StoredLayout in_flash = store::host_record();

  // Power cycle: RAM is gone (the "prepared" flag is clear again) but the flash
  // record is still there. init() must find and report it.
  reset_flash();
  store::host_record() = in_flash;
  CHECK(store::init());

  moto_ui_layout_t loaded{};
  std::uint16_t revision = 0;
  CHECK(store::load(&loaded, &revision));
  CHECK(revision == 77);
  CHECK(std::memcmp(&loaded, &expected, sizeof(expected)) == 0);
}

void test_factory_reset_clears_the_record() {
  reset_flash();
  moto_ui_layout_t expected = layout_fixture();
  CHECK(store::save(&expected, 11));
  CHECK(store::saved());

  store::factory_reset();
  CHECK(!store::saved());
  CHECK(!store::init());

  moto_ui_layout_t loaded = layout_fixture();
  CHECK(!store::load(&loaded, nullptr));

  // The store is usable again after a reset.
  CHECK(store::save(&expected, 12));
  CHECK(store::saved());
  std::uint16_t revision = 0;
  CHECK(store::load(&loaded, &revision));
  CHECK(revision == 12);

  // A reset on a virgin device is harmless.
  reset_flash();
  store::factory_reset();
  CHECK(!store::saved());
  CHECK(!store::init());
}

void test_crc_corruption_is_rejected() {
  // record_crc() covers every byte before the crc32 field, so corruption
  // anywhere in the record — header, payload head, or payload tail (component
  // slots 4..8) — is caught structurally instead of relying on valid_layout().

  // A single flipped bit in the record header (revision).
  reset_flash();
  moto_ui_layout_t expected = layout_fixture();
  CHECK(store::save(&expected, 5));
  store::host_record().revision ^= 0x0001U;
  moto_ui_layout_t loaded = layout_fixture();
  CHECK(!store::load(&loaded, nullptr));
  CHECK(!store::saved());
  CHECK(!store::init());

  // A single flipped bit inside the checksummed payload prefix. The corrupted
  // opacity (80 -> 81) is still semantically valid, so only the CRC can catch
  // it.
  reset_flash();
  CHECK(store::save(&expected, 5));
  store::host_record().layout.map_opacity ^= 0x01U;
  CHECK(store::valid_layout(store::host_record().layout));
  CHECK(!store::check_record(store::host_record()));
  CHECK(!store::load(&loaded, nullptr));

  // The same corruption in the tail of the payload must be caught too. This
  // slot sits past the kCrcChunkBytes run boundary that an earlier
  // implementation stopped at, so a regression here would silently accept a
  // corrupted component.
  static_assert(sizeof(moto_ui_layout_t) > store::kCrcChunkBytes,
                "the payload must be large enough to cross a hashing run");
  reset_flash();
  CHECK(store::save(&expected, 5));
  {
    moto_ui_layout_t& tail = store::host_record().layout;
    // components[5] is well past the first 64 bytes of the record.
    tail.components[5].x = static_cast<std::int16_t>(tail.components[5].x + 1);
    CHECK(store::valid_layout(tail));
    CHECK(!store::check_record(store::host_record()));
    CHECK(!store::load(&loaded, nullptr));
  }

  // The very last payload byte (the span of component 8) is the extreme case of
  // the same defect: a legal-looking arc span must still fail the CRC.
  reset_flash();
  CHECK(store::save(&expected, 5));
  {
    moto_ui_layout_t& tail = store::host_record().layout;
    const auto last = static_cast<std::size_t>(tail.component_count) - 1;
    tail.components[last].span_deg = 180;  // fixture uses 90; both are legal
    CHECK(store::valid_layout(tail));
    CHECK(!store::check_record(store::host_record()));
    CHECK(!store::load(&loaded, nullptr));
  }

  // A damaged CRC field itself.
  reset_flash();
  CHECK(store::save(&expected, 5));
  store::host_record().crc32 ^= 0x80000000U;
  CHECK(!store::load(&loaded, nullptr));

  // A stale CRC left behind by an interrupted write.
  reset_flash();
  CHECK(store::save(&expected, 5));
  store::host_record().crc32 = 0;
  CHECK(!store::load(&loaded, nullptr));
}

void test_wrong_magic_and_length_are_rejected() {
  moto_ui_layout_t loaded = layout_fixture();

  // Zero magic ('M' 'U' 'I' '1' expected). The CRC is recomputed so the magic
  // check is what rejects the record.
  reset_flash();
  store::StoredLayout record = valid_record(3);
  record.magic = 0;
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(!store::check_record(store::host_record()));
  CHECK(!store::load(&loaded, nullptr));
  CHECK(!store::saved());

  // A newer schema version 'MUI2' must not be read with the old field
  // meanings, even though its CRC is valid.
  record = valid_record(3);
  record.magic = 0x3249554DU;
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(!store::check_record(store::host_record()));
  CHECK(!store::load(&loaded, nullptr));
  CHECK(!store::init());

  // Wrong payload length, with a CRC that matches the shortened record.
  record = valid_record(3);
  record.length = static_cast<std::uint16_t>(sizeof(moto_ui_layout_t) - 1U);
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(!store::check_record(store::host_record()));
  CHECK(!store::load(&loaded, nullptr));

  record = valid_record(3);
  record.length = 0;
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(!store::load(&loaded, nullptr));

  record = valid_record(3);
  record.length = 0xFFFFU;
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(!store::load(&loaded, nullptr));
}

void test_torn_write_is_rejected_not_clamped() {
  reset_flash();

  // Erased flash: a zeroed record. The caller keeps its own default layout and
  // must not receive a partly-clamped one.
  place_record(store::StoredLayout{});
  moto_ui_layout_t loaded = layout_fixture();
  loaded.map_opacity = 7;
  CHECK(!store::load(&loaded, nullptr));
  CHECK(loaded.map_opacity == 7);
  CHECK(!store::saved());
  CHECK(!store::init());

  // Header written, payload not yet programmed.
  store::StoredLayout header_only{};
  header_only.magic = store::kStoredMagic;
  header_only.length = static_cast<std::uint16_t>(sizeof(moto_ui_layout_t));
  place_record(header_only);
  CHECK(!store::load(&loaded, nullptr));

  // Payload still erased while the CRC of the previous write survives.
  store::StoredLayout stale = valid_record();
  stale.layout = moto_ui_layout_t{};
  place_record(stale);
  CHECK(!store::check_record(store::host_record()));
  CHECK(!store::load(&loaded, nullptr));

  // Truncated write: the first 48 payload bytes are still erased while the
  // tail (and the CRC of the complete write) made it to flash.
  store::StoredLayout torn = valid_record();
  std::memset(&torn.layout, 0, 48);
  place_record(torn);
  CHECK(!store::load(&loaded, nullptr));
  CHECK(!store::saved());

  // A later complete save recovers the store.
  moto_ui_layout_t recovered = layout_fixture();
  CHECK(store::save(&recovered, 21));
  CHECK(store::saved());
  CHECK(store::load(&loaded, nullptr));
}

// --- valid_layout(): accepted reference layouts ----------------------------

void test_reference_layouts_are_valid() {
  CHECK(store::valid_layout(layout_fixture()));
  CHECK(store::valid_layout(single_component_layout(MOTO_UI_COMPONENT_SPEED)));
  CHECK(
      store::valid_layout(single_component_layout(MOTO_UI_COMPONENT_SPEED_ARC)));
  CHECK(store::valid_layout(single_component_layout(MOTO_UI_COMPONENT_SPEED_LIMIT)));
}

// --- valid_layout(): per-field bounds -------------------------------------

void test_opacity_bounds() {
  moto_ui_layout_t layout = layout_fixture();
  layout.map_opacity = 101;
  CHECK(!store::valid_layout(layout));

  layout = layout_fixture();
  layout.route_opacity = 101;
  CHECK(!store::valid_layout(layout));

  layout = layout_fixture();
  layout.street_opacity = 101;
  CHECK(!store::valid_layout(layout));

  layout = layout_fixture();
  layout.building_opacity = 101;
  CHECK(!store::valid_layout(layout));

  // 0..100 is the documented range, 100 included.
  layout = layout_fixture();
  layout.map_opacity = 100;
  layout.route_opacity = 100;
  layout.street_opacity = 100;
  layout.building_opacity = 100;
  CHECK(store::valid_layout(layout));

  layout = layout_fixture();
  layout.map_opacity = 0;
  CHECK(store::valid_layout(layout));

  // Component opacity shares the same range.
  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].opacity = 101;
  CHECK(!store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].opacity = 100;
  CHECK(store::valid_layout(layout));
}

void test_size_and_theme_bounds() {
  // size: 0 S / 1 M / 2 L.
  for (std::uint8_t size = 0; size <= MOTO_UI_SIZE_L; ++size) {
    moto_ui_layout_t layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
    layout.components[0].size = size;
    CHECK(store::valid_layout(layout));
  }
  moto_ui_layout_t layout =
      single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].size = 3;
  CHECK(!store::valid_layout(layout));

  // theme_id: 0 dark / 1 light / 2 high contrast / 3 custom.
  for (std::uint8_t theme = 0; theme <= 3; ++theme) {
    layout = layout_fixture();
    layout.theme_id = theme;
    CHECK(store::valid_layout(layout));
  }
  layout = layout_fixture();
  layout.theme_id = 4;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.theme_id = 255;
  CHECK(!store::valid_layout(layout));
}

void test_component_id_bounds_and_duplicates() {
  // Protocol §4.2 ids are exactly 1..8.
  for (std::uint8_t id = 1; id <= MOTO_UI_LAYOUT_COMPONENT_COUNT; ++id) {
    CHECK(store::valid_layout(single_component_layout(id)));
  }
  CHECK(!store::valid_layout(single_component_layout(0)));
  CHECK(!store::valid_layout(single_component_layout(9)));
  CHECK(!store::valid_layout(single_component_layout(255)));

  // Duplicate ids inside one layout are a protocol violation.
  moto_ui_layout_t layout = layout_fixture();
  layout.component_count = 2;
  layout.components[1].id = layout.components[0].id;
  CHECK(!store::valid_layout(layout));

  layout = layout_fixture();
  layout.component_count = 2;
  layout.components[0].id = MOTO_UI_COMPONENT_SPEED_ARC;
  layout.components[0].x = 0;
  layout.components[0].y = 0;
  layout.components[0].anchor = MOTO_UI_ARC_TOP;
  layout.components[0].span_deg = 180;
  layout.components[1].id = MOTO_UI_COMPONENT_SPEED_ARC;
  layout.components[1].x = 0;
  layout.components[1].y = 0;
  layout.components[1].anchor = MOTO_UI_ARC_TOP;
  layout.components[1].span_deg = 180;
  CHECK(!store::valid_layout(layout));
}

void test_arc_anchor_span_and_position() {
  // Every documented anchor (0 top / 1 right / 2 bottom / 3 left) is accepted.
  for (std::uint8_t anchor = MOTO_UI_ARC_TOP; anchor <= MOTO_UI_ARC_LEFT;
       ++anchor) {
    moto_ui_layout_t layout =
        single_component_layout(MOTO_UI_COMPONENT_PROGRESS_ARC);
    layout.components[0].anchor = anchor;
    CHECK(store::valid_layout(layout));
  }
  moto_ui_layout_t layout =
      single_component_layout(MOTO_UI_COMPONENT_PROGRESS_ARC);
  layout.components[0].anchor = 4;
  CHECK(!store::valid_layout(layout));
  layout = single_component_layout(MOTO_UI_COMPONENT_PROGRESS_ARC);
  layout.components[0].anchor = 255;
  CHECK(!store::valid_layout(layout));

  // The three arc spans.
  const std::uint8_t valid_spans[] = {90, 120, 180};
  for (std::uint8_t span : valid_spans) {
    layout = single_component_layout(MOTO_UI_COMPONENT_SPEED_ARC);
    layout.components[0].span_deg = span;
    CHECK(store::valid_layout(layout));
  }
  // span_deg is a byte on the wire, so 255 is the largest possible value.
  const std::uint8_t invalid_spans[] = {0, 45, 89, 121, 179, 255};
  for (std::uint8_t span : invalid_spans) {
    layout = single_component_layout(MOTO_UI_COMPONENT_SPEED_ARC);
    layout.components[0].span_deg = span;
    CHECK(!store::valid_layout(layout));
  }

  // Arcs are anchored, never positioned.
  layout = single_component_layout(MOTO_UI_COMPONENT_PROGRESS_ARC);
  layout.components[0].x = 1;
  CHECK(!store::valid_layout(layout));
  layout = single_component_layout(MOTO_UI_COMPONENT_PROGRESS_ARC);
  layout.components[0].y = 1;
  CHECK(!store::valid_layout(layout));
  layout = single_component_layout(MOTO_UI_COMPONENT_PROGRESS_ARC);
  layout.components[0].x = -1;
  CHECK(!store::valid_layout(layout));

  // Widgets must not carry arc-only fields.
  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].anchor = MOTO_UI_ARC_TOP + 1;
  CHECK(!store::valid_layout(layout));
  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].span_deg = 90;
  CHECK(!store::valid_layout(layout));
}

void test_widget_position_bounds() {
  // The canvas is 466 x 466, so 0..465 is the inclusive valid range.
  moto_ui_layout_t layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].x = 0;
  layout.components[0].y = 0;
  CHECK(store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].x = MOTO_UI_CANVAS_WIDTH - 1;
  layout.components[0].y = MOTO_UI_CANVAS_HEIGHT - 1;
  CHECK(store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].x = MOTO_UI_CANVAS_WIDTH;
  CHECK(!store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].y = MOTO_UI_CANVAS_HEIGHT;
  CHECK(!store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].x = -1;
  CHECK(!store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].y = -1;
  CHECK(!store::valid_layout(layout));

  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].x = 32767;
  layout.components[0].y = 32767;
  CHECK(!store::valid_layout(layout));
}

void test_map_height_bounds() {
  // 160..466 and even. Out of range, and odd values inside the range.
  const std::uint16_t invalid_heights[] = {0, 159, 161, 233, 32767, 465, 467};
  for (std::uint16_t height : invalid_heights) {
    moto_ui_layout_t layout = layout_fixture();
    layout.map_height_px = height;
    CHECK(!store::valid_layout(layout));
  }

  const std::uint16_t valid_heights[] = {MOTO_UI_LAYOUT_MIN_MAP_HEIGHT,
                                         MOTO_UI_LAYOUT_DEFAULT_MAP_HEIGHT,
                                         MOTO_UI_LAYOUT_MAX_MAP_HEIGHT};
  for (std::uint16_t height : valid_heights) {
    moto_ui_layout_t layout = layout_fixture();
    layout.map_height_px = height;
    CHECK(store::valid_layout(layout));
  }
}

void test_zoom_layer_mask_and_count_bounds() {
  // zoom_percent: 50..200.
  moto_ui_layout_t layout = layout_fixture();
  layout.zoom_percent = 49;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.zoom_percent = 201;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.zoom_percent = 50;
  CHECK(store::valid_layout(layout));
  layout = layout_fixture();
  layout.zoom_percent = 200;
  CHECK(store::valid_layout(layout));

  // layer_mask: bit0 roads, bit1 buildings, bit2 route, nothing else.
  const std::uint8_t invalid_masks[] = {0x08U, 0x10U, 0x80U, 0xFFU};
  for (std::uint8_t mask : invalid_masks) {
    layout = layout_fixture();
    layout.layer_mask = mask;
    CHECK(!store::valid_layout(layout));
  }
  const std::uint8_t valid_masks[] = {0x00U, 0x01U, 0x07U};
  for (std::uint8_t mask : valid_masks) {
    layout = layout_fixture();
    layout.layer_mask = mask;
    CHECK(store::valid_layout(layout));
  }

  // component_count: 0..8 (8 is the fixed array size).
  layout = layout_fixture();
  layout.component_count = 0;
  CHECK(store::valid_layout(layout));
  layout = layout_fixture();
  layout.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT;
  CHECK(store::valid_layout(layout));
  layout = layout_fixture();
  layout.component_count = MOTO_UI_LAYOUT_COMPONENT_COUNT + 1;
  CHECK(!store::valid_layout(layout));
}

void test_flag_and_width_bounds() {
  // Boolean wire fields are 0/1 only.
  moto_ui_layout_t layout = layout_fixture();
  layout.north_up = 2;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.north_up = 255;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.north_up = 0;
  CHECK(store::valid_layout(layout));

  layout = layout_fixture();
  layout.music_page_enabled = 2;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.music_page_enabled = 0;
  CHECK(store::valid_layout(layout));

  layout = layout_fixture();
  layout.map_visible = 2;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.map_visible = 0;
  CHECK(store::valid_layout(layout));

  // route_width_px: 1..16; street_width_px: 1..12.
  layout = layout_fixture();
  layout.route_width_px = 0;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.route_width_px = 17;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.route_width_px = 1;
  CHECK(store::valid_layout(layout));
  layout = layout_fixture();
  layout.route_width_px = 16;
  CHECK(store::valid_layout(layout));

  layout = layout_fixture();
  layout.street_width_px = 0;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.street_width_px = 13;
  CHECK(!store::valid_layout(layout));
  layout = layout_fixture();
  layout.street_width_px = 1;
  CHECK(store::valid_layout(layout));
  layout = layout_fixture();
  layout.street_width_px = 12;
  CHECK(store::valid_layout(layout));

  // Component enabled flag is bit0 of the wire cflags; nothing else may be set.
  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].enabled = 2;
  CHECK(!store::valid_layout(layout));
  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].enabled = 1;
  CHECK(store::valid_layout(layout));
  layout = single_component_layout(MOTO_UI_COMPONENT_SPEED);
  layout.components[0].enabled = 0;
  CHECK(store::valid_layout(layout));
}

// --- cross-layer gates -----------------------------------------------------

void test_valid_crc_with_invalid_payload_is_rejected() {
  // check_record() only proves structural integrity. A record can pass the CRC
  // and still describe something the phone never asked for, so flash_read()
  // must apply valid_layout() as a second gate instead of clamping.

  // In-range zoom is required; 10 is not.
  reset_flash();
  store::StoredLayout record = valid_record(9);
  record.layout.zoom_percent = 10;
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(store::check_record(store::host_record()));
  CHECK(!store::valid_layout(store::host_record().layout));

  moto_ui_layout_t loaded = layout_fixture();
  std::uint16_t revision = 0xFFFFU;
  CHECK(!store::load(&loaded, &revision));
  CHECK(revision == 0xFFFFU);
  CHECK(!store::saved());
  CHECK(!store::init());

  // Duplicate component ids with a perfectly valid CRC.
  record = valid_record(9);
  record.layout.components[1].id = record.layout.components[0].id;
  record.crc32 = store::record_crc(record);
  place_record(record);
  CHECK(store::check_record(store::host_record()));
  CHECK(!store::load(&loaded, nullptr));

  // A semantically rejected record must not be reported as saved by init().
  CHECK(!store::init());
}

void test_save_refuses_invalid_layout() {
  reset_flash();
  moto_ui_layout_t invalid = layout_fixture();
  invalid.map_height_px = 161;  // odd
  CHECK(!store::save(&invalid, 1));
  CHECK(!store::saved());

  moto_ui_layout_t loaded = layout_fixture();
  CHECK(!store::load(&loaded, nullptr));
  CHECK(!store::save(nullptr, 1));

  // A refused save must not overwrite the layout already in flash.
  const moto_ui_layout_t good = layout_fixture();
  CHECK(store::save(&good, 30));
  CHECK(!store::save(&invalid, 31));
  std::uint16_t revision = 0;
  CHECK(store::load(&loaded, &revision));
  CHECK(revision == 30);
  CHECK(std::memcmp(&loaded, &good, sizeof(good)) == 0);
}

void test_empty_component_list_round_trip() {
  reset_flash();
  moto_ui_layout_t empty = layout_fixture();
  empty.component_count = 0;
  CHECK(store::valid_layout(empty));
  CHECK(store::save(&empty, 40));

  moto_ui_layout_t loaded{};
  std::uint16_t revision = 0;
  CHECK(store::load(&loaded, &revision));
  CHECK(revision == 40);
  CHECK(loaded.component_count == 0);
  CHECK(std::memcmp(&loaded, &empty, sizeof(empty)) == 0);
}

}  // namespace

int main() {
  test_crc32_contract();
  test_fresh_device_has_no_saved_layout();
  test_save_and_load_round_trip();
  test_persisted_record_survives_restart();
  test_factory_reset_clears_the_record();
  test_crc_corruption_is_rejected();
  test_wrong_magic_and_length_are_rejected();
  test_torn_write_is_rejected_not_clamped();
  test_reference_layouts_are_valid();
  test_opacity_bounds();
  test_size_and_theme_bounds();
  test_component_id_bounds_and_duplicates();
  test_arc_anchor_span_and_position();
  test_widget_position_bounds();
  test_map_height_bounds();
  test_zoom_layer_mask_and_count_bounds();
  test_flag_and_width_bounds();
  test_valid_crc_with_invalid_payload_is_rejected();
  test_save_refuses_invalid_layout();
  test_empty_component_list_round_trip();

  if (failures != 0) {
    std::cerr << failures << " UI layout store checks failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All UI layout store checks passed\n";
  return EXIT_SUCCESS;
}
