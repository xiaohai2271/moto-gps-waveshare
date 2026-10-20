#include "headless_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace moto::sim {
namespace {

lv_display_t* g_display = nullptr;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void write_u16(std::ofstream& out, std::uint16_t value) {
  const char bytes[2] = {static_cast<char>(value & 0xFFU),
                         static_cast<char>((value >> 8U) & 0xFFU)};
  out.write(bytes, 2);
}

void write_u32(std::ofstream& out, std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value & 0xFFU),
                         static_cast<char>((value >> 8U) & 0xFFU),
                         static_cast<char>((value >> 16U) & 0xFFU),
                         static_cast<char>((value >> 24U) & 0xFFU)};
  out.write(bytes, 4);
}

}  // namespace

bool HeadlessDisplay::init(std::string* error) {
  if(display_ != nullptr) {
    return true;
  }

  lv_init();

  shadow_.assign(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel,
                 0);
  draw_buffer_.assign(
      static_cast<std::size_t>(kWidth) * kDrawBufferRows * kBytesPerPixel, 0);

  display_ = lv_display_create(kWidth, kHeight);
  if(display_ == nullptr) {
    if(error != nullptr) *error = "lv_display_create returned null";
    return false;
  }
  lv_display_set_user_data(display_, this);
  lv_display_set_flush_cb(display_, &HeadlessDisplay::flush);
  lv_display_set_buffers(display_, draw_buffer_.data(), nullptr,
                         static_cast<std::uint32_t>(draw_buffer_.size()),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  g_display = display_;
  return true;
}

void HeadlessDisplay::advance(std::uint32_t elapsed_ms) {
  if(display_ == nullptr) {
    return;
  }
  lv_tick_inc(elapsed_ms);
}

void HeadlessDisplay::flush(lv_display_t* display, const lv_area_t* area,
                            std::uint8_t* pixels) {
  auto* self =
      static_cast<HeadlessDisplay*>(lv_display_get_user_data(display));
  if(self != nullptr && area != nullptr && pixels != nullptr) {
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    if(width > 0 && height > 0 && area->x1 >= 0 && area->y1 >= 0 &&
       area->x2 < kWidth && area->y2 < kHeight) {
      const std::size_t row_bytes =
          static_cast<std::size_t>(width) * kBytesPerPixel;
      for(int y = 0; y < height; ++y) {
        const std::size_t source = static_cast<std::size_t>(y) * row_bytes;
        const std::size_t target =
            (static_cast<std::size_t>(area->y1 + y) * kWidth +
             static_cast<std::size_t>(area->x1)) *
            kBytesPerPixel;
        std::memcpy(self->shadow_.data() + target, pixels + source,
                    row_bytes);
      }
    }
  }
  lv_display_flush_ready(display);
}

std::uint64_t HeadlessDisplay::frame_hash() const {
  std::uint64_t hash = kFnvOffset;
  for(const std::uint8_t byte : shadow_) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
  return hash;
}

bool HeadlessDisplay::save_bmp(const std::string& path,
                               std::string* error) const {
  if(shadow_.empty()) {
    if(error != nullptr) *error = "display has not been initialised";
    return false;
  }

  constexpr int kRowBytes = kWidth * 3;
  constexpr int kPadding = (4 - (kRowBytes % 4)) % 4;
  constexpr int kRowStride = kRowBytes + kPadding;
  constexpr std::uint32_t kPixelBytes =
      static_cast<std::uint32_t>(kRowStride) * kHeight;
  constexpr std::uint32_t kFileSize = 54U + kPixelBytes;

  std::ofstream out(path, std::ios::binary);
  if(!out) {
    if(error != nullptr) *error = "cannot open " + path;
    return false;
  }

  // BITMAPFILEHEADER
  out.write("BM", 2);
  write_u32(out, kFileSize);
  write_u16(out, 0);
  write_u16(out, 0);
  write_u32(out, 54);

  // BITMAPINFOHEADER
  write_u32(out, 40);
  write_u32(out, static_cast<std::uint32_t>(kWidth));
  write_u32(out, static_cast<std::uint32_t>(kHeight));
  write_u16(out, 1);
  write_u16(out, 24);
  write_u32(out, 0);  // BI_RGB
  write_u32(out, kPixelBytes);
  write_u32(out, 2835);  // ~72 dpi
  write_u32(out, 2835);
  write_u32(out, 0);
  write_u32(out, 0);

  const char zero[4] = {0, 0, 0, 0};
  std::vector<char> row(static_cast<std::size_t>(kRowStride), 0);
  for(int y = kHeight - 1; y >= 0; --y) {
    const std::uint8_t* source =
        shadow_.data() + static_cast<std::size_t>(y) * kWidth * kBytesPerPixel;
    for(int x = 0; x < kWidth; ++x) {
      const std::uint16_t value = static_cast<std::uint16_t>(
          source[static_cast<std::size_t>(x) * 2] |
          (source[static_cast<std::size_t>(x) * 2 + 1] << 8));
      const std::uint8_t r = static_cast<std::uint8_t>((value >> 11U) & 0x1FU);
      const std::uint8_t g = static_cast<std::uint8_t>((value >> 5U) & 0x3FU);
      const std::uint8_t b = static_cast<std::uint8_t>(value & 0x1FU);
      row[static_cast<std::size_t>(x) * 3 + 0] =
          static_cast<char>((b << 3U) | (b >> 2U));
      row[static_cast<std::size_t>(x) * 3 + 1] =
          static_cast<char>((g << 2U) | (g >> 4U));
      row[static_cast<std::size_t>(x) * 3 + 2] =
          static_cast<char>((r << 3U) | (r >> 2U));
    }
    if(kPadding > 0) {
      std::memcpy(row.data() + kRowBytes, zero,
                  static_cast<std::size_t>(kPadding));
    }
    out.write(row.data(), row.size());
    if(!out) {
      if(error != nullptr) *error = "write failed for " + path;
      return false;
    }
  }
  return true;
}

lv_display_t* display_handle() { return g_display; }

}  // namespace moto::sim
