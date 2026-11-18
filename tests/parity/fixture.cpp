// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "fixture.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;

namespace birdie::test {
namespace {

[[noreturn]] void fail(const fs::path& file, const std::string& what) {
  throw std::runtime_error(file.string() + ": " + what);
}

json read_json(const fs::path& file) {
  std::ifstream f(file, std::ios::binary);
  if (!f) fail(file, "cannot open");
  try {
    return json::parse(f);
  } catch (const json::exception& e) {
    fail(file, e.what());
  }
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  std::string cell;
  std::istringstream s(line);
  while (std::getline(s, cell, ',')) out.push_back(cell);
  if (!line.empty() && line.back() == ',') out.emplace_back();
  return out;
}

double to_double(const std::string& cell, const fs::path& file, size_t line_no) {
  double v = 0;
  const char* b = cell.data();
  const char* e = b + cell.size();
  auto [p, ec] = std::from_chars(b, e, v);
  if (ec != std::errc() || p != e)
    fail(file, "line " + std::to_string(line_no) + ": not a number: '" + cell + "'");
  return v;
}

}  // namespace

size_t Fixture::col(const std::string& col_name) const {
  auto it = std::find(columns.begin(), columns.end(), col_name);
  if (it == columns.end()) throw std::runtime_error(col_name + ": no such input column in " + name);
  return static_cast<size_t>(it - columns.begin());
}

Fixture load_fixture(const fs::path& dir) {
  Fixture fx;
  fx.dir = dir;
  fx.meta = read_json(dir / "fixture.json");
  fx.name = fx.meta.value("name", dir.filename().string());

  // input.csv
  const fs::path csv = dir / fx.meta.at("input").value("file", std::string("input.csv"));
  std::ifstream f(csv, std::ios::binary);
  if (!f) fail(csv, "cannot open");
  std::string line;
  size_t line_no = 0;
  while (std::getline(f, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto cells = split(line);
    if (fx.columns.empty()) {
      fx.columns = std::move(cells);
      if (fx.columns.empty() || fx.columns[0] != "t") fail(csv, "first column must be 't'");
      continue;
    }
    if (cells.size() != fx.columns.size())
      fail(csv, "line " + std::to_string(line_no) + ": " + std::to_string(cells.size()) + " cells, header has " +
                    std::to_string(fx.columns.size()));
    std::vector<double> row(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) row[i] = to_double(cells[i], csv, line_no);
    if (!fx.rows.empty() && row[0] <= fx.rows.back()[0])
      fail(csv, "line " + std::to_string(line_no) + ": t is not increasing");
    fx.rows.push_back(std::move(row));
  }
  if (fx.rows.empty()) fail(csv, "no data rows");

  // checks + goldens
  for (const auto& c : fx.meta.at("checks")) {
    Check ck;
    ck.index = c.at("index").get<int>();
    ck.t = c.at("t").get<double>();
    ck.video_s = c.value("video_s", 0.0);
    char idx[16];
    std::snprintf(idx, sizeof idx, "%03d", ck.index);
    const fs::path gj = dir / "golden" / (std::string(idx) + ".json");
    ck.golden = read_json(gj);
    ck.golden_png = dir / "golden" / (std::string(idx) + ".png");
    if (!fs::exists(ck.golden_png)) fail(ck.golden_png, "missing");
    if (std::abs(ck.golden.at("t").get<double>() - ck.t) > 1e-6) fail(gj, "t differs from fixture.json");

    // The reference captures a check at the first row with t >= check t (t is rounded to 1e-4 in the files).
    auto it = std::lower_bound(fx.rows.begin(), fx.rows.end(), ck.t - 5e-4,
                               [](const std::vector<double>& r, double t) { return r[0] < t; });
    if (it == fx.rows.end() || std::abs((*it)[0] - ck.t) > 5e-4)
      fail(gj, "no input row at t=" + std::to_string(ck.t));
    ck.row = static_cast<size_t>(it - fx.rows.begin());
    fx.checks.push_back(std::move(ck));
  }
  return fx;
}

std::vector<fs::path> find_fixtures(const fs::path& root) {
  std::vector<fs::path> out;
  if (!fs::is_directory(root)) return out;
  for (const auto& e : fs::directory_iterator(root))
    if (e.is_directory() && fs::exists(e.path() / "fixture.json")) out.push_back(e.path());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace birdie::test
