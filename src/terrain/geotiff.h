// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Copernicus DEM GeoTIFF -> Birdie's height tile (.bdem), without GDAL.
//
// The Copernicus DEM tiles (GLO-30 / GLO-90, AWS buckets copernicus-dem-30m / -90m, or the same files from the
// Copernicus Data Space) are Cloud-Optimised GeoTIFFs: one band of 32-bit floats (metres above the EGM2008 geoid),
// 1024 / 2048 px tiles, DEFLATE with the floating-point predictor (3). Read here: little-endian classic TIFF, tiled or
// in strips, compression none / DEFLATE, predictor none / horizontal / floating point, float32 or int16 samples, the
// first image only (the overviews after it are skipped), ModelPixelScale + ModelTiepoint for the georeference.
//
// A .bdem file is what the terrain code maps into memory (terrain/terrain.h):
//   header, little-endian, 56 bytes: char[4] "BDEM", u32 version = 1, u32 width, u32 height,
//     f64 lat0, f64 lon0 (centre of the first sample; row 0 = north edge, column 0 = west edge),
//     f64 dlat, f64 dlon (spacing in degrees, dlat > 0: rows go south), u32 reserved[2]
//   int16 heights[height][width], metres (rounded); -32768 = no data
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace birdie::terrain {

struct BdemInfo {
  std::string name;  // tile name from the georeference, N34W119
  int res_m = 0;     // 30 or 90
  int w = 0, h = 0;
};

// Converts one GeoTIFF held in memory and writes `out_dir / <name>.bdem` (via a .tmp, then renamed). Returns false
// with *err set when the file is not something it can read.
bool geotiff_to_bdem(const std::vector<uint8_t>& tif, const std::filesystem::path& out_dir, BdemInfo* info,
                     std::string* err);

// The .bdem header's resolution in metres (30 / 90), 0 = not a readable .bdem.
int bdem_res(const std::filesystem::path& file);

}  // namespace birdie::terrain
