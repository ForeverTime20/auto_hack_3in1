#include "games.h"

#include "../capture/game_window.h"
#include "../input/key_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

namespace gta5::games::match {
namespace {

using Frame = gta5::capture::GameFrame;
using Clock = std::chrono::steady_clock;

struct RectI {
  int left = 0, top = 0, right = 0, bottom = 0;
  int width() const { return right - left; }
  int height() const { return bottom - top; }
  int centerX() const { return (left + right) / 2; }
  int centerY() const { return (top + bottom) / 2; }
};

// Coordinates are full-frame analysis pixels. Layout contains every solver ROI.
struct Geometry {
  double scale = 0;
  std::array<RectI, 3> targetDigits{};
  std::array<RectI, 3> leftDigits{};
  std::array<RectI, 3> multiplierIcons{};
  std::uint64_t windowGeneration = 0;
  int frameWidth = 0;
  int frameHeight = 0;
};

struct Rgb { int r = 0, g = 0, b = 0; };
struct VisualState {
  int leftCurrent = -1;
  int completedMask = 0;
  int usedMask = 0;
  bool operator==(const VisualState& other) const {
    return leftCurrent == other.leftCurrent && completedMask == other.completedMask &&
           usedMask == other.usedMask;
  }
};

std::optional<Geometry> g_detectedGeometry;

std::uint32_t Pixel(const Frame& frame, int x, int y) {
  if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) return 0;
  return frame.bgra[static_cast<std::size_t>(y) * frame.width + x];
}

Rgb Color(std::uint32_t pixel) {
  return {static_cast<int>((pixel >> 16) & 255), static_cast<int>((pixel >> 8) & 255),
          static_cast<int>(pixel & 255)};
}

double NeutralBrightness(const Frame& frame, int x, int y) {
  const Rgb c = Color(Pixel(frame, x, y));
  return std::min({c.r, c.g, c.b});
}

double MeanNeutralHorizontal(const Frame& frame, int y, int left, int right) {
  left = std::clamp(left, 0, frame.width);
  right = std::clamp(right, 0, frame.width);
  if (y < 0 || y >= frame.height || right <= left) return 0;
  double sum = 0;
  for (int x = left; x < right; ++x) sum += NeutralBrightness(frame, x, y);
  return sum / (right - left);
}

double MeanNeutralVertical(const Frame& frame, int x, int top, int bottom) {
  top = std::clamp(top, 0, frame.height);
  bottom = std::clamp(bottom, 0, frame.height);
  if (x < 0 || x >= frame.width || bottom <= top) return 0;
  double sum = 0;
  for (int y = top; y < bottom; ++y) sum += NeutralBrightness(frame, x, y);
  return sum / (bottom - top);
}

bool AnchorsPresent(const Frame& frame, const Geometry& geometry) {
  if (geometry.targetDigits[0].height() <= 0) return false;
  const int delta = std::max(2, static_cast<int>(std::lround(frame.height * .004)));
  const int top = geometry.targetDigits[0].top;
  const int bottom = geometry.targetDigits[0].bottom;
  const int left = geometry.targetDigits[0].left - 1;
  const int right = geometry.targetDigits[2].right + 1;
  const int inset = std::max(2, delta);
  const double topContrast = MeanNeutralHorizontal(frame, top, left + inset, right - inset) -
      (MeanNeutralHorizontal(frame, top - delta, left + inset, right - inset) +
       MeanNeutralHorizontal(frame, top + delta, left + inset, right - inset)) * .5;
  const double bottomContrast = MeanNeutralHorizontal(frame, bottom, left + inset, right - inset) -
      (MeanNeutralHorizontal(frame, bottom - delta, left + inset, right - inset) +
       MeanNeutralHorizontal(frame, bottom + delta, left + inset, right - inset)) * .5;
  if (topContrast + bottomContrast < 30) return false;
  double verticalContrast = 0;
  for (int i = 0; i < 4; ++i) {
    const int x = i == 0 ? left : geometry.targetDigits[i - 1].right + 1;
    verticalContrast += MeanNeutralVertical(frame, x, top + inset, bottom - inset) -
        (MeanNeutralVertical(frame, x - delta, top + inset, bottom - inset) +
         MeanNeutralVertical(frame, x + delta, top + inset, bottom - inset)) * .5;
  }
  return verticalContrast >= 45;
}

std::optional<Geometry> LocateGeometry(const Frame& frame) {
  if (frame.width < 640 || frame.height < 360 ||
      frame.bgra.size() != static_cast<std::size_t>(frame.width) * frame.height) return std::nullopt;
  const int centerX = frame.width / 2;
  const int delta = std::max(2, static_cast<int>(std::lround(frame.height * .004)));
  const int hx1 = std::max(0, static_cast<int>(std::lround(centerX - frame.height * .16)));
  const int hx2 = std::min(frame.width, static_cast<int>(std::lround(centerX + frame.height * .16)));
  const int hy1 = std::max(delta, static_cast<int>(std::lround(frame.height * .05)));
  const int hy2 = std::min(frame.height - delta - 1,
                           static_cast<int>(std::lround(frame.height * .30)));
  std::vector<double> horizontal(frame.height), contrast(frame.height);
  for (int y = hy1 - delta; y <= hy2 + delta; ++y)
    horizontal[y] = MeanNeutralHorizontal(frame, y, hx1, hx2);
  for (int y = hy1; y <= hy2; ++y)
    contrast[y] = horizontal[y] - (horizontal[y - delta] + horizontal[y + delta]) * .5;

  int top = -1, bottom = -1;
  double bestHorizontal = 0;
  for (int y1 = hy1 + 1; y1 < hy2; ++y1) {
    if (contrast[y1] < contrast[y1 - 1] || contrast[y1] <= contrast[y1 + 1]) continue;
    for (int y2 = y1 + 1; y2 < hy2; ++y2) {
      const double separation = (y2 - y1) / static_cast<double>(frame.height);
      if (separation < .085 || separation > .13 ||
          contrast[y2] < contrast[y2 - 1] || contrast[y2] <= contrast[y2 + 1]) continue;
      const double score = contrast[y1] + contrast[y2];
      if (score > bestHorizontal) { bestHorizontal = score; top = y1; bottom = y2; }
    }
  }
  if (top < 0 || bestHorizontal < 35) return std::nullopt;

  const int vx1 = std::max(delta, static_cast<int>(std::lround(centerX - frame.height * .22)));
  const int vx2 = std::min(frame.width - delta - 1,
                           static_cast<int>(std::lround(centerX + frame.height * .22)));
  std::vector<double> vertical(frame.width), vContrast(frame.width);
  for (int x = vx1 - delta; x <= vx2 + delta; ++x)
    vertical[x] = MeanNeutralVertical(frame, x, top, bottom + 1);
  std::vector<int> peaks;
  for (int x = vx1; x <= vx2; ++x)
    vContrast[x] = vertical[x] - (vertical[x - delta] + vertical[x + delta]) * .5;
  for (int x = vx1 + 1; x < vx2; ++x) {
    if (vContrast[x] <= 15 || vContrast[x] < vContrast[x - 1] ||
        vContrast[x] <= vContrast[x + 1]) continue;
    if (!peaks.empty() && x - peaks.back() <= 2) {
      if (vContrast[x] > vContrast[peaks.back()]) peaks.back() = x;
    } else peaks.push_back(x);
  }

  std::array<int, 4> edges{};
  double bestVertical = 0;
  for (int first : peaks) for (int last : peaks) {
    if (last <= first) continue;
    const double pitch = (last - first) / 3.0;
    if (pitch / frame.height < .055 || pitch / frame.height > .105) continue;
    std::array<int, 4> candidate{};
    double error = 0;
    for (int i = 0; i < 4; ++i) {
      const double expected = first + pitch * i;
      candidate[i] = *std::min_element(peaks.begin(), peaks.end(), [=](int a, int b) {
        return std::abs(a - expected) < std::abs(b - expected);
      });
      error += std::abs(candidate[i] - expected) / pitch;
    }
    if (error > .15 || candidate[0] == candidate[1] || candidate[1] == candidate[2] ||
        candidate[2] == candidate[3]) continue;
    double score = -20 * error;
    for (int edge : candidate) score += vContrast[edge];
    score -= std::abs((first + last) * .5 - centerX) * .05;
    if (score > bestVertical) { bestVertical = score; edges = candidate; }
  }
  if (bestVertical < 60) return std::nullopt;

  Geometry geometry;
  geometry.scale = ((bottom - top) / 116.0 +
                    (edges[3] - edges[0]) / (3.0 * 89.0)) * .5;
  if (geometry.scale <= .25) return std::nullopt;
  for (int i = 0; i < 3; ++i)
    geometry.targetDigits[i] = {edges[i] + 1, top, edges[i + 1] - 1, bottom};

  const int frameH = bottom - top;
  const int frameW = static_cast<int>(std::lround((edges[3] - edges[0]) / 3.0));
  const int edgeOffset = std::max(3, static_cast<int>(std::lround(frameH * .05)));
  auto maximum = [&](int x, int y) { const Rgb c = Color(Pixel(frame, x, y)); return std::max({c.r,c.g,c.b}); };
  auto verticalLine = [&](int x, int y, int height) {
    if (x < 1 || x + 1 >= frame.width || y < 0 || y + height > frame.height) return 0.0;
    double sum = 0;
    for (int py = y; py < y + height; ++py)
      sum += std::max({maximum(x - 1, py), maximum(x, py), maximum(x + 1, py)});
    return sum / height;
  };
  auto horizontalLine = [&](int x, int y, int width) {
    if (y < 1 || y + 1 >= frame.height || x < 0 || x + width > frame.width) return 0.0;
    double sum = 0;
    for (int px = x; px < x + width; ++px)
      sum += std::max({maximum(px, y - 1), maximum(px, y), maximum(px, y + 1)});
    return sum / width;
  };
  auto rectangleScore = [&](int x, int y) {
    if (x - edgeOffset < 1 || x + frameW + edgeOffset >= frame.width ||
        y - edgeOffset < 1 || y + frameH + edgeOffset >= frame.height) return -1.0;
    const double edge = verticalLine(x, y, frameH) + verticalLine(x + frameW, y, frameH) +
                        horizontalLine(x, y, frameW) + horizontalLine(x, y + frameH, frameW);
    const double nearby = (verticalLine(x - edgeOffset, y, frameH) +
        verticalLine(x + edgeOffset, y, frameH) + verticalLine(x + frameW - edgeOffset, y, frameH) +
        verticalLine(x + frameW + edgeOffset, y, frameH) + horizontalLine(x, y - edgeOffset, frameW) +
        horizontalLine(x, y + edgeOffset, frameW) + horizontalLine(x, y + frameH - edgeOffset, frameW) +
        horizontalLine(x, y + frameH + edgeOffset, frameW)) * .5;
    return edge - nearby;
  };

  for (int row = 0; row < 3; ++row) {
    const int expectedX = static_cast<int>(std::lround(edges[0] - frameH * 3.04));
    const int expectedY = static_cast<int>(std::lround(top + frameH * (1.27 + row * 2.01)));
    const int radiusX = static_cast<int>(std::lround(frameH * .45));
    const int radiusY = static_cast<int>(std::lround(frameH * .25));
    const int coarse = std::max(1, static_cast<int>(std::lround(geometry.scale * 2)));
    double bestScore = -1; int bestX = 0, bestY = 0;
    for (int y = expectedY - radiusY; y <= expectedY + radiusY; y += coarse)
      for (int x = expectedX - radiusX; x <= expectedX + radiusX; x += coarse) {
        const double score = rectangleScore(x, y);
        if (score > bestScore) { bestScore = score; bestX = x; bestY = y; }
      }
    for (int y = bestY - coarse; y <= bestY + coarse; ++y)
      for (int x = bestX - coarse; x <= bestX + coarse; ++x) {
        const double score = rectangleScore(x, y);
        if (score > bestScore) { bestScore = score; bestX = x; bestY = y; }
      }
    if (bestScore < 80) return std::nullopt;
    geometry.leftDigits[row] = {bestX, bestY, bestX + frameW, bestY + frameH};
  }

  constexpr double kPi = 3.14159265358979323846;
  auto ringScore = [&](int cx, int cy, int radius) {
    double edge = 0, inside = 0, outside = 0;
    const int radialOffset = std::max(3, static_cast<int>(std::lround(frameH * .05)));
    for (int i = 0; i < 32; ++i) {
      const double angle = 2 * kPi * i / 32, cs = std::cos(angle), sn = std::sin(angle);
      edge += maximum(static_cast<int>(std::lround(cx + radius * cs)), static_cast<int>(std::lround(cy + radius * sn)));
      inside += maximum(static_cast<int>(std::lround(cx + (radius - radialOffset) * cs)),
                        static_cast<int>(std::lround(cy + (radius - radialOffset) * sn)));
      outside += maximum(static_cast<int>(std::lround(cx + (radius + radialOffset) * cs)),
                         static_cast<int>(std::lround(cy + (radius + radialOffset) * sn)));
    }
    return (edge - (inside + outside) * .5) / 32;
  };
  for (int row = 0; row < 3; ++row) {
    const int expectedX = static_cast<int>(std::lround(edges[3] + frameH * 2.55));
    const int expectedY = geometry.leftDigits[row].centerY();
    const int searchX = static_cast<int>(std::lround(frameH * .70));
    const int searchY = static_cast<int>(std::lround(frameH * .18));
    const int minRadius = static_cast<int>(std::lround(frameH * .43));
    const int maxRadius = static_cast<int>(std::lround(frameH * .58));
    const int step = std::max(1, static_cast<int>(std::lround(geometry.scale * 2)));
    double bestScore = -1; int bestX = 0, bestY = 0, bestRadius = 0;
    for (int cy = expectedY - searchY; cy <= expectedY + searchY; cy += step)
      for (int cx = expectedX - searchX; cx <= expectedX + searchX; cx += step)
        for (int radius = minRadius; radius <= maxRadius; radius += step) {
          const double score = ringScore(cx, cy, radius);
          if (score > bestScore) { bestScore = score; bestX = cx; bestY = cy; bestRadius = radius; }
        }
    if (bestScore < 12) return std::nullopt;
    const int halfW = static_cast<int>(std::lround(bestRadius * .78));
    const int halfH = static_cast<int>(std::lround(bestRadius * .82));
    geometry.multiplierIcons[row] = {bestX - halfW, bestY - halfH, bestX + halfW, bestY + halfH};
  }
  geometry.windowGeneration = frame.windowGeneration;
  geometry.frameWidth = frame.width;
  geometry.frameHeight = frame.height;
  return geometry;
}

bool IsBright(Rgb c) { return std::max({c.r, c.g, c.b}) >= 145 && c.r + c.g + c.b >= 300; }

double BrightRatio(const Frame& frame, RectI roi) {
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  int bright = 0, total = 0;
  for (int y = roi.top; y < roi.bottom; ++y) for (int x = roi.left; x < roi.right; ++x) {
    bright += IsBright(Color(Pixel(frame, x, y))); ++total;
  }
  return total ? bright / static_cast<double>(total) : 0;
}

int ReadSevenSegment(const Frame& frame, const RectI& box) {
  constexpr std::array<std::array<double, 4>, 7> strips{{
      {{.36,.17,.64,.24}}, {{.66,.27,.77,.43}}, {{.66,.58,.77,.74}},
      {{.36,.77,.64,.84}}, {{.25,.58,.36,.74}}, {{.25,.27,.36,.43}},
      {{.36,.47,.64,.55}}}};
  int mask = 0;
  for (int i = 0; i < 7; ++i) {
    const auto& s = strips[i];
    const RectI roi{static_cast<int>(std::lround(box.left + s[0] * box.width())),
                    static_cast<int>(std::lround(box.top + s[1] * box.height())),
                    static_cast<int>(std::lround(box.left + s[2] * box.width())),
                    static_cast<int>(std::lround(box.top + s[3] * box.height()))};
    if (BrightRatio(frame, roi) > .12) mask |= 1 << i;
  }
  constexpr std::array<int, 10> masks{0b0111111,0b0000110,0b1011011,0b1001111,0b1100110,
                                      0b1101101,0b1111101,0b0000111,0b1111111,0b1101111};
  int best = -1, bestDistance = 8;
  for (int digit = 0; digit < 10; ++digit) {
    unsigned difference = static_cast<unsigned>(mask ^ masks[digit]); int distance = 0;
    while (difference) { distance += difference & 1U; difference >>= 1U; }
    if (distance < bestDistance) { bestDistance = distance; best = digit; }
  }
  return bestDistance <= 1 ? best : -1;
}

int ReadMultiplier(const Frame& frame, const Geometry& geometry, int row) {
  RectI roi = geometry.multiplierIcons[row];
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  int minX = roi.right, minY = roi.bottom, maxX = -1, maxY = -1, count = 0;
  for (int y = roi.top; y < roi.bottom; ++y) for (int x = roi.left; x < roi.right; ++x) {
    const Rgb c = Color(Pixel(frame, x, y));
    if (std::min({c.r,c.g,c.b}) < 155 || c.r + c.g + c.b < 570) continue;
    minX = std::min(minX, x); minY = std::min(minY, y);
    maxX = std::max(maxX, x); maxY = std::max(maxY, y); ++count;
  }
  if (count < std::max(12, static_cast<int>(180 * geometry.scale * geometry.scale))) return -1;
  const double width = (maxX - minX + 1) / geometry.scale;
  const double height = (maxY - minY + 1) / geometry.scale;
  if (height < 52) return 2;
  if (width < 57) return 1;
  return 10;
}

bool ReadPuzzle(const Frame& frame, const Geometry& geometry, std::array<int, 3>& values,
                std::array<int, 3>& multipliers, int& target) {
  for (int row = 0; row < 3; ++row) {
    values[row] = ReadSevenSegment(frame, geometry.leftDigits[row]);
    multipliers[row] = ReadMultiplier(frame, geometry, row);
    if (values[row] < 0 || multipliers[row] < 0) return false;
  }
  const int d0 = ReadSevenSegment(frame, geometry.targetDigits[0]);
  const int d1 = ReadSevenSegment(frame, geometry.targetDigits[1]);
  const int d2 = ReadSevenSegment(frame, geometry.targetDigits[2]);
  if (d0 < 0 || d1 < 0 || d2 < 0) return false;
  target = d0 * 100 + d1 * 10 + d2;
  return true;
}

double MosaicChromaScore(const Frame& frame, RectI roi, int block) {
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  block = std::max(1, block);
  std::vector<double> scores;
  for (int by = roi.top; by + block <= roi.bottom; by += block)
    for (int bx = roi.left; bx + block <= roi.right; bx += block) {
      double r = 0, g = 0, b = 0;
      for (int y = by; y < by + block; ++y) for (int x = bx; x < bx + block; ++x) {
        const Rgb c = Color(Pixel(frame, x, y)); r += c.r; g += c.g; b += c.b;
      }
      const double count = block * block;
      r /= count; g /= count; b /= count;
      const double high = std::max({r,g,b}), low = std::min({r,g,b});
      if (high > 25) scores.push_back((high - low) / (high + 20));
    }
  if (scores.empty()) return 0;
  std::sort(scores.begin(), scores.end(), std::greater<double>());
  const std::size_t take = std::max<std::size_t>(1, scores.size() / 5);
  return std::accumulate(scores.begin(), scores.begin() + take, 0.0) / take;
}

int UniqueColoredRow(const std::array<double, 3>& scores) {
  int best = 0;
  for (int row = 1; row < 3; ++row) if (scores[row] > scores[best]) best = row;
  double second = 0;
  for (int row = 0; row < 3; ++row) if (row != best) second = std::max(second, scores[row]);
  return scores[best] > .18 && scores[best] - second > .12 ? best : -1;
}

VisualState ReadVisualState(const Frame& frame, const Geometry& geometry) {
  std::array<double, 3> leftScores{};
  VisualState state;
  const int block = std::max(1, static_cast<int>(std::lround(geometry.scale * 4)));
  const int wireBlock = std::max(1, static_cast<int>(std::lround(geometry.scale * 2)));
  for (int row = 0; row < 3; ++row) {
    const RectI& number = geometry.leftDigits[row];
    const int h = number.height();
    const int margin = std::max(2, static_cast<int>(std::lround(h * .06)));
    leftScores[row] = MosaicChromaScore(frame,
        {number.left-margin,number.top-margin,number.right+margin,number.bottom+margin}, block);
    const int halfWire = std::max(2, static_cast<int>(std::lround(h * .06)));
    const double leftWire = MosaicChromaScore(frame,
        {number.right + static_cast<int>(std::lround(h * .37)), number.centerY()-halfWire,
         number.right + static_cast<int>(std::lround(h * .67)), number.centerY()+halfWire}, wireBlock);
    const RectI& icon = geometry.multiplierIcons[row];
    const int circleRadius = static_cast<int>(std::lround(icon.width() / (2 * .78)));
    const int circleLeft = icon.centerX() - circleRadius;
    const double rightWire = MosaicChromaScore(frame,
        {circleLeft - static_cast<int>(std::lround(h * .69)), icon.centerY()-halfWire,
         circleLeft - static_cast<int>(std::lround(h * .34)), icon.centerY()+halfWire}, wireBlock);
    if (leftWire > .18) state.completedMask |= 1 << row;
    if (rightWire > .18) state.usedMask |= 1 << row;
  }
  state.leftCurrent = UniqueColoredRow(leftScores);
  return state;
}

int BitCount3(int value) { return (value & 1) + ((value >> 1) & 1) + ((value >> 2) & 1); }

int NextAvailableRow(int row, int usedMask) {
  for (int step = 1; step <= 3; ++step) {
    const int candidate = (row + step) % 3;
    if (!(usedMask & (1 << candidate))) return candidate;
  }
  return -1;
}

std::optional<std::array<int, 3>> MakeSolution(const std::array<int, 3>& values,
                                               const std::array<int, 3>& multipliers,
                                               int target) {
  std::array<int, 3> rows{0,1,2};
  do {
    int sum = 0;
    for (int i = 0; i < 3; ++i) sum += values[i] * multipliers[rows[i]];
    if (sum == target) return rows;
  } while (std::next_permutation(rows.begin(), rows.end()));
  return std::nullopt;
}

bool SameLayout(const Geometry& a, const Geometry& b) {
  const RectI& ar = a.targetDigits[0]; const RectI& br = b.targetDigits[0];
  return std::abs(ar.left-br.left) <= 2 && std::abs(ar.top-br.top) <= 2 &&
         std::abs(a.targetDigits[2].right-b.targetDigits[2].right) <= 2 &&
         std::abs(ar.bottom-br.bottom) <= 2;
}

bool MatchesFrame(const Geometry& geometry, const Frame& frame) {
  return geometry.windowGeneration == frame.windowGeneration &&
         geometry.frameWidth == frame.width && geometry.frameHeight == frame.height;
}

HWND ForegroundGameWindow(const Frame& frame) {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return nullptr;
  RECT client{}; POINT origin{};
  if (!GetClientRect(foreground, &client) || !ClientToScreen(foreground, &origin)) return nullptr;
  return origin.x == frame.screenX && origin.y == frame.screenY &&
         client.right-client.left == frame.screenW && client.bottom-client.top == frame.screenH
      ? foreground : nullptr;
}

void WaitFrame(const std::function<bool()>& stopRequested, Clock::time_point started) {
  const auto deadline = started + std::chrono::milliseconds(50);
  while (!stopRequested() && Clock::now() < deadline) {
    if (deadline - Clock::now() > std::chrono::milliseconds(2)) Sleep(1);
    else std::this_thread::yield();
  }
}

}  // namespace

bool DetectInGame() {
  Frame frame;
  if (!gta5::capture::CaptureGameFrame(frame)) { g_detectedGeometry.reset(); return false; }
  g_detectedGeometry = LocateGeometry(frame);
  return g_detectedGeometry.has_value();
}

void ResetInGameCache() { g_detectedGeometry.reset(); }

bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status) {
  std::optional<Geometry> geometry = g_detectedGeometry;
  std::optional<std::array<int, 3>> solution;
  gta5::input::Job inputJob;
  VisualState state{}, previous{};
  int absentFrames = 0, stableFrames = 0;
  bool havePrevious = false, awaitingTransition = false, stateReady = false;
  int oldCompleted = 0, oldUsed = 0, connectingLeft = -1, connectingRight = -1;
  auto transitionDeadline = Clock::time_point{};
  std::wstring lastStatus;
  auto setStatus = [&](const wchar_t* value) {
    if (lastStatus != value) { lastStatus = value; status(value); }
  };
  auto resetPlan = [&] {
    solution.reset(); awaitingTransition = false; stateReady = false;
    stableFrames = 0; havePrevious = false;
  };
  auto cleanup = [&] {
    gta5::input::CancelAll(); inputJob = {}; geometry.reset(); ResetInGameCache();
  };

  setStatus(L"match: locating");
  while (!stopRequested()) {
    const auto started = Clock::now();
    Frame frame;
    if (!gta5::capture::CaptureGameFrame(frame)) {
      geometry.reset(); g_detectedGeometry.reset(); resetPlan();
      if (++absentFrames >= 3) break;
      WaitFrame(stopRequested, started); continue;
    }

    bool needsRelocation = !geometry || !MatchesFrame(*geometry, frame) || !AnchorsPresent(frame, *geometry);
    if (needsRelocation) {
      const auto relocated = LocateGeometry(frame);  // Same-frame full-search fallback.
      if (!relocated) {
        geometry.reset(); g_detectedGeometry.reset();
        if (++absentFrames >= 3) {
          if (inputJob && BitCount3(oldCompleted) == 2) {
            if (inputJob.Pending()) { WaitFrame(stopRequested, started); continue; }
            if (inputJob.Succeeded()) {
              setStatus(L"match: completed"); cleanup(); return true;
            }
          }
          if (awaitingTransition && BitCount3(oldCompleted) == 2) {
            setStatus(L"match: completed"); cleanup(); return true;
          }
          setStatus(L"match: minigame exited"); break;
        }
        WaitFrame(stopRequested, started); continue;
      }
      if (!geometry || !SameLayout(*geometry, *relocated) || !MatchesFrame(*geometry, frame)) {
        gta5::input::CancelAll(); inputJob = {}; resetPlan();
      }
      geometry = relocated; g_detectedGeometry = relocated;
    }
    absentFrames = 0;

    if (inputJob) {
      if (inputJob.Pending()) { WaitFrame(stopRequested, started); continue; }
      if (!inputJob.Succeeded()) {
        inputJob = {}; resetPlan(); setStatus(L"match: analyzing");
        WaitFrame(stopRequested, started); continue;
      }
      inputJob = {};
      awaitingTransition = true;
      transitionDeadline = Clock::now() + std::chrono::seconds(6);
      stableFrames = 0; havePrevious = false;
      setStatus(L"match: verifying connection");
    }

    state = ReadVisualState(frame, *geometry);
    if (awaitingTransition) {
      const bool finalPair = BitCount3(oldCompleted) == 2;
      const bool connected = (state.completedMask & (1 << connectingLeft)) &&
                             (state.usedMask & (1 << connectingRight)) &&
                             state.completedMask != oldCompleted && state.usedMask != oldUsed;
      const bool advanced = finalPair || (state.leftCurrent >= 0 && state.leftCurrent != connectingLeft);
      if (connected && advanced && havePrevious && state == previous) ++stableFrames;
      else stableFrames = connected && advanced ? 1 : 0;
      previous = state; havePrevious = true;
      if (stableFrames >= 3) {
        awaitingTransition = false; stableFrames = 0; havePrevious = false;
        stateReady = true;
        if (BitCount3(state.completedMask) == 3) {
          setStatus(L"match: completed"); cleanup(); return true;
        }
        setStatus(L"match: connecting");
      } else if (Clock::now() >= transitionDeadline) {
        resetPlan(); setStatus(L"match: analyzing");
      } else {
        WaitFrame(stopRequested, started); continue;
      }
    }

    if (!solution) {
      setStatus(L"match: reading puzzle");
      std::array<int, 3> values{}, multipliers{}; int target = 0;
      if (!ReadPuzzle(frame, *geometry, values, multipliers, target)) {
        WaitFrame(stopRequested, started); continue;
      }
      solution = MakeSolution(values, multipliers, target);
      if (!solution) {
        setStatus(L"match: no solution"); cleanup(); return false;
      }
      setStatus(L"match: connecting");
    }

    if (!stateReady) {
      if (havePrevious && state == previous) ++stableFrames;
      else stableFrames = 1;
      previous = state; havePrevious = true;
      if (stableFrames < 3) { WaitFrame(stopRequested, started); continue; }
      stateReady = true; stableFrames = 0; havePrevious = false;
    }

    if (BitCount3(state.completedMask) == 3) {
      setStatus(L"match: completed"); cleanup(); return true;
    }
    int leftRow = state.leftCurrent;
    if (leftRow < 0)
      for (int row = 0; row < 3; ++row) if (!(state.completedMask & (1 << row))) { leftRow = row; break; }
    if (leftRow < 0 || (state.completedMask & (1 << leftRow))) {
      WaitFrame(stopRequested, started); continue;
    }
    const int targetRight = (*solution)[leftRow];
    int selectedRight = (state.usedMask & (1 << leftRow))
        ? NextAvailableRow(leftRow, state.usedMask) : leftRow;
    std::vector<gta5::input::Key> keys{gta5::input::Key::FromVirtualKey(VK_RETURN)};
    for (int moves = 0; selectedRight != targetRight && moves < 3; ++moves) {
      keys.push_back(gta5::input::Key::FromVirtualKey(VK_DOWN));
      selectedRight = NextAvailableRow(selectedRight, state.usedMask);
    }
    if (selectedRight != targetRight) { resetPlan(); WaitFrame(stopRequested, started); continue; }
    keys.push_back(gta5::input::Key::FromVirtualKey(VK_RETURN));
    const HWND foreground = ForegroundGameWindow(frame);
    if (!foreground) { WaitFrame(stopRequested, started); continue; }
    oldCompleted = state.completedMask; oldUsed = state.usedMask;
    connectingLeft = leftRow; connectingRight = targetRight;
    inputJob = gta5::input::QueueSequence(keys, foreground);
    stateReady = false;
    setStatus(L"match: connecting");
    WaitFrame(stopRequested, started);
  }

  cleanup();
  return false;
}

}  // namespace gta5::games::match
