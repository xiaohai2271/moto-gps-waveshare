#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"

namespace moto::sim {

// Headless 466x466 RGB565 display for the virtual device. Frames are rendered
// by the real shared LVGL UI, stitched into a shadow framebuffer (partial
// render areas included) and can be written as a 24-bit BMP for inspection.
class HeadlessDisplay {
 public:
  bool init(std::string* error);

  [[nodiscard]] lv_display_t* handle() const { return display_; }

  // Feeds LVGL's millisecond tick. Call once per virtual frame.
  void advance(std::uint32_t elapsed_ms);

  [[nodiscard]] bool save_bmp(const std::string& path,
                              std::string* error) const;

  [[nodiscard]] const std::vector<std::uint8_t>& frame() const {
    return shadow_;
  }
  [[nodiscard]] std::uint64_t frame_hash() const;

  static constexpr int kWidth = 466;
  static constexpr int kHeight = 466;
  static constexpr int kBytesPerPixel = 2;
  static constexpr int kDrawBufferRows = 32;

 private:
  static void flush(lv_display_t* display, const lv_area_t* area,
                    std::uint8_t* pixels);

  lv_display_t* display_ = nullptr;
  std::vector<std::uint8_t> shadow_;
  std::vector<std::uint8_t> draw_buffer_;
};

// The single display created by HeadlessDisplay, for board_port_get_display().
lv_display_t* display_handle();

}  // namespace moto::sim
