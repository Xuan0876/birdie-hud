// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "terrain/geotiff.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include "miniz.h"
#include "terrain/terrain.h"

namespace birdie::terrain {
namespace {

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint32_t version, w, h;
  double lat0, lon0, dlat, dlon;
  uint32_t reserved[2];
};
#pragma pack(pop)
static_assert(sizeof(Header) == 56);

struct Reader {
  const std::vector<uint8_t>& b;
  bool ok = true;
  template <class T>
  T at(size_t off) {
    T v{};
    if (off + sizeof(T) > b.size()) {
      ok = false;
      return v;
    }
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
  }
};

// One IFD entry's values as numbers (SHORT, LONG, DOUBLE; inline when they fit in 4 bytes).
struct Tag {
  uint16_t type = 0;
  uint32_t count = 0;
  uint32_t value = 0;  // the raw 4 bytes: the value itself or the offset
};

std::vector<double> values(Reader& r, const Tag& t) {
  const size_t sz = t.type == 3 ? 2 : t.type == 4 ? 4 : t.type == 12 ? 8 : t.type == 16 ? 8 : 0;
  std::vector<double> v;
  if (!sz || t.count > (1u << 24)) return v;
  const bool inl = sz * t.count <= 4;
  const size_t base = t.value;
  for (uint32_t i = 0; i < t.count; ++i) {
    if (inl) {
      v.push_back(t.type == 3 ? static_cast<double>((t.value >> (16 * i)) & 0xffff) : t.value);
    } else {
      const size_t o = base + i * sz;
      if (t.type == 3) v.push_back(r.at<uint16_t>(o));
      else if (t.type == 4) v.push_back(r.at<uint32_t>(o));
      else if (t.type == 12) v.push_back(r.at<double>(o));
      else v.push_back(static_cast<double>(r.at<uint64_t>(o)));
    }
  }
  return v;
}

bool fail(std::string* err, const char* what) {
  if (err) *err = what;
  return false;
}

}  // namespace

bool geotiff_to_bdem(const std::vector<uint8_t>& tif, const std::filesystem::path& out_dir, BdemInfo* info,
                     std::string* err) {
  Reader r{tif};
  if (tif.size() < 8 || tif[0] != 'I' || tif[1] != 'I') return fail(err, "not a little-endian TIFF");
  if (r.at<uint16_t>(2) != 42) return fail(err, "BigTIFF / not a TIFF");
  const uint32_t ifd = r.at<uint32_t>(4);
  const uint16_t n = r.at<uint16_t>(ifd);
  if (!r.ok || n == 0 || n > 200) return fail(err, "bad TIFF directory");
  std::map<uint16_t, Tag> tags;
  for (uint16_t i = 0; i < n; ++i) {
    const size_t e = ifd + 2 + 12 * size_t(i);
    tags[r.at<uint16_t>(e)] = {r.at<uint16_t>(e + 2), r.at<uint32_t>(e + 4), r.at<uint32_t>(e + 8)};
  }
  if (!r.ok) return fail(err, "truncated TIFF directory");
  auto one = [&](uint16_t id, double def) {
    const auto it = tags.find(id);
    if (it == tags.end()) return def;
    const auto v = values(r, it->second);
    return v.empty() ? def : v[0];
  };
  auto list = [&](uint16_t id) {
    const auto it = tags.find(id);
    return it == tags.end() ? std::vector<double>{} : values(r, it->second);
  };
  const int W = static_cast<int>(one(256, 0)), H = static_cast<int>(one(257, 0));
  const int bps = static_cast<int>(one(258, 0)), compr = static_cast<int>(one(259, 1));
  const int spp = static_cast<int>(one(277, 1)), pred = static_cast<int>(one(317, 1));
  const int fmt = static_cast<int>(one(339, 1));
  if (W < 2 || H < 2 || W > 20000 || H > 20000) return fail(err, "odd image size");
  if (spp != 1) return fail(err, "more than one band");
  const bool f32 = bps == 32 && fmt == 3, i16 = bps == 16 && (fmt == 1 || fmt == 2);
  if (!f32 && !i16) return fail(err, "samples are neither float32 nor int16");
  if (compr != 1 && compr != 8 && compr != 32946) return fail(err, "compression not supported (only none / DEFLATE)");
  if (pred != 1 && pred != 2 && pred != 3) return fail(err, "predictor not supported");
  const int bytes = bps / 8;
  // tiles, or strips as tiles one image wide
  const bool tiled = tags.count(322) && tags.count(323);
  const int tw = tiled ? static_cast<int>(one(322, 0)) : W;
  const int th = tiled ? static_cast<int>(one(323, 0)) : static_cast<int>(one(278, H));
  const auto offs = list(tiled ? 324 : 273), lens = list(tiled ? 325 : 279);
  if (tw < 1 || th < 1 || offs.empty() || offs.size() != lens.size()) return fail(err, "no image data");
  const int across = (W + tw - 1) / tw, down = (H + th - 1) / std::max(1, th);
  if (static_cast<int>(offs.size()) < across * down) return fail(err, "too few tiles");
  // georeference
  const auto scale = list(33550), tie = list(33922), keys = list(34735);
  if (scale.size() < 2 || tie.size() < 6) return fail(err, "no georeference");
  const double dlon = scale[0], dlat = scale[1];
  int raster_type = 1;  // GTRasterTypeGeoKey: 1 = pixel is area (tie point = corner), 2 = pixel is point (= centre)
  for (size_t k = 4; k + 3 < keys.size(); k += 4)
    if (static_cast<int>(keys[k]) == 1025 && keys[k + 1] == 0) raster_type = static_cast<int>(keys[k + 3]);
  const double lon_t = tie[3] - tie[0] * dlon, lat_t = tie[4] + tie[1] * dlat;
  const double lat0 = raster_type == 1 ? lat_t - dlat / 2 : lat_t, lon0 = raster_type == 1 ? lon_t + dlon / 2 : lon_t;
  if (!(dlat > 0 && dlat < 0.01 && dlon > 0 && dlon < 0.05) || std::abs(lat0) > 90 || std::abs(lon0) > 180)
    return fail(err, "georeference is not degrees");
  const int ilat = static_cast<int>(std::floor(lat0 - (H - 1) * dlat + 1e-6));
  const int ilon = static_cast<int>(std::floor(lon0 + 1e-6));
  const int res = dlat < 0.0005 ? 30 : 90;

  std::vector<int16_t> out(static_cast<size_t>(W) * H, -32768);
  std::vector<uint8_t> raw(static_cast<size_t>(tw) * th * bytes), row(static_cast<size_t>(tw) * bytes);
  for (int ty = 0; ty < down; ++ty)
    for (int tx = 0; tx < across; ++tx) {
      const size_t k = static_cast<size_t>(ty) * across + tx;
      const size_t off = static_cast<size_t>(offs[k]), len = static_cast<size_t>(lens[k]);
      if (off + len > tif.size()) return fail(err, "tile outside the file");
      const int rows = tiled ? th : std::min(th, H - ty * th);
      const size_t want = static_cast<size_t>(tw) * rows * bytes;
      if (compr == 1) {
        if (len < want) return fail(err, "short tile");
        std::memcpy(raw.data(), tif.data() + off, want);
      } else {
        mz_ulong got = static_cast<mz_ulong>(want);
        if (mz_uncompress(raw.data(), &got, tif.data() + off, static_cast<mz_ulong>(len)) != MZ_OK || got != want)
          return fail(err, "DEFLATE data is broken");
      }
      for (int y = 0; y < rows; ++y) {
        uint8_t* p = raw.data() + static_cast<size_t>(y) * tw * bytes;
        if (pred == 3) {  // floating point: byte-wise differences, then the byte planes (most significant first)
          for (size_t i = 1; i < row.size(); ++i) p[i] = static_cast<uint8_t>(p[i] + p[i - 1]);
          for (int x = 0; x < tw; ++x)
            for (int b = 0; b < bytes; ++b) row[static_cast<size_t>(x) * bytes + (bytes - 1 - b)] = p[static_cast<size_t>(b) * tw + x];
          std::memcpy(p, row.data(), row.size());
        } else if (pred == 2) {  // horizontal differences of whole samples
          if (f32) return fail(err, "horizontal predictor on floats");
          int16_t* s = reinterpret_cast<int16_t*>(p);
          for (int x = 1; x < tw; ++x) s[x] = static_cast<int16_t>(s[x] + s[x - 1]);
        }
        const int gy = ty * th + y;
        if (gy >= H) break;
        for (int x = 0; x < tw; ++x) {
          const int gx = tx * tw + x;
          if (gx >= W) break;
          double v;
          if (f32) {
            float f;
            std::memcpy(&f, p + static_cast<size_t>(x) * 4, 4);
            v = f;
          } else {
            int16_t s;
            std::memcpy(&s, p + static_cast<size_t>(x) * 2, 2);
            v = s;
          }
          if (std::isfinite(v) && v > -1000 && v < 9000)
            out[static_cast<size_t>(gy) * W + gx] = static_cast<int16_t>(std::lround(std::clamp(v, -32767.0, 32767.0)));
        }
      }
    }

  const std::string name = tile_name(ilat, ilon);
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  const auto file = out_dir / (name + ".bdem");
  const auto tmp = out_dir / (name + ".bdem.tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    Header hd{{'B', 'D', 'E', 'M'}, 1, static_cast<uint32_t>(W), static_cast<uint32_t>(H), lat0, lon0, dlat, dlon, {0, 0}};
    f.write(reinterpret_cast<const char*>(&hd), sizeof hd);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * 2));
    if (!f) {
      f.close();
      std::filesystem::remove(tmp, ec);
      return fail(err, "cannot write the tile (disk full?)");
    }
  }
  std::filesystem::rename(tmp, file, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return fail(err, "cannot replace the tile (in use?)");
  }
  if (info) *info = {name, res, W, H};
  return true;
}

int bdem_res(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  Header hd{};
  if (!f.read(reinterpret_cast<char*>(&hd), sizeof hd) || std::memcmp(hd.magic, "BDEM", 4) != 0) return 0;
  return hd.dlat < 0.0005 ? 30 : 90;
}

}  // namespace birdie::terrain
