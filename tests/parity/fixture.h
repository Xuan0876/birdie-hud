// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Loads a parity fixture (tests/README.md): fixture.json, input.csv, golden/NNN.json.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace birdie::test {

struct Check {
  int index = 0;
  double t = 0;        // replay time, s from the first input row
  double video_s = 0;  // time in the original recording (for humans)
  size_t row = 0;      // input row at which the reference captured this check
  nlohmann::json golden;             // golden/NNN.json
  std::filesystem::path golden_png;  // golden/NNN.png
};

struct Fixture {
  std::string name;
  std::filesystem::path dir;
  nlohmann::json meta;               // fixture.json
  std::vector<std::string> columns;  // input.csv header ("t" first)
  std::vector<std::vector<double>> rows;
  std::vector<Check> checks;

  // Column index by name; throws if missing.
  size_t col(const std::string& col_name) const;
  double at(size_t row, const std::string& col_name) const { return rows[row][col(col_name)]; }
};

// Throws std::runtime_error with a readable message on any format problem.
Fixture load_fixture(const std::filesystem::path& dir);

// All subdirectories of `root` that contain a fixture.json, sorted by name.
std::vector<std::filesystem::path> find_fixtures(const std::filesystem::path& root);

}  // namespace birdie::test
