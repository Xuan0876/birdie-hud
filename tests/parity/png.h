// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace birdie::test {

struct Image {
  int w = 0, h = 0;
  std::vector<std::uint8_t> px;  // RGBA rows
};

bool load_png_rgba(const std::filesystem::path& file, Image& img);
bool save_png_rgba(const std::filesystem::path& file, const Image& img);

}  // namespace birdie::test
