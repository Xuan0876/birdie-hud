// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Minimal 2D/3D vector and 3x3 matrix types (double precision, like the numpy reference).
#pragma once

#include <cmath>
#include <optional>
#include <vector>

namespace birdie {

struct Vec2 {
  double x = 0, y = 0;
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(double k, Vec2 a) { return {k * a.x, k * a.y}; }
inline bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

using Polyline = std::vector<Vec2>;
using OptVec2 = std::optional<Vec2>;

struct Vec3 {
  double x = 0, y = 0, z = 0;
  double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(double k, Vec3 a) { return {k * a.x, k * a.y, k * a.z}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm_xy(Vec3 a) { return std::hypot(a.x, a.y); }

// 3x3 matrix stored by columns (numpy column_stack): c[0], c[1], c[2].
struct Mat3 {
  Vec3 c[3];
  static Mat3 from_columns(Vec3 a, Vec3 b, Vec3 d) { return {{a, b, d}}; }
  Vec3 row(int i) const { return {c[0][i], c[1][i], c[2][i]}; }
  Vec3 operator*(Vec3 v) const { return v.x * c[0] + v.y * c[1] + v.z * c[2]; }
  // transpose(M) * v
  Vec3 tmul(Vec3 v) const { return {dot(c[0], v), dot(c[1], v), dot(c[2], v)}; }
  Mat3 operator*(const Mat3& o) const { return from_columns((*this) * o.c[0], (*this) * o.c[1], (*this) * o.c[2]); }
  Mat3 transposed() const { return from_columns(row(0), row(1), row(2)); }
};

// numpy.linspace(a, b, n) (endpoint included, last element exactly b)
inline std::vector<double> linspace(double a, double b, int n) {
  std::vector<double> out(n);
  if (n == 1) {
    out[0] = a;
    return out;
  }
  const double step = (b - a) / (n - 1);
  for (int i = 0; i < n; ++i) out[i] = i * step + a;
  out[n - 1] = b;
  return out;
}

// Python's floored modulo for floats and ints.
inline double pymod(double a, double b) {
  double r = std::fmod(a, b);
  if (r != 0 && ((r < 0) != (b < 0))) r += b;
  return r;
}
inline long long pymod(long long a, long long b) {
  long long r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) r += b;
  return r;
}
inline long long pyfloordiv(long long a, long long b) { return (a - pymod(a, b)) / b; }
inline double wrap180(double d) { return pymod(d + 180.0, 360.0) - 180.0; }

constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * (kPi / 180.0); }
inline double deg(double r) { return r * (180.0 / kPi); }

}  // namespace birdie
