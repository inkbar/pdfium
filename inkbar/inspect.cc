// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See inspect.h. The page check walks the page's objects, as the redaction
// does, and uses the redaction's own rules wherever it can: a glyph counts
// when its box meets a region on an open interval (cpdf_text_redactor), an
// image is read over the sample grid the apply blanks (cpdf_image_redactor),
// and a path or shading counts when PDFium's path redactor would cut it. So
// content the apply rightly kept does not count.

#include "inkbar/inspect.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fpdfapi/edit/cpdf_image_redactor.h"
#include "core/fpdfapi/edit/cpdf_path_redactor.h"
#include "core/fpdfapi/edit/cpdf_text_redactor.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/font/cpdf_type3char.h"
#include "core/fpdfapi/font/cpdf_type3font.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorstate.h"
#include "core/fpdfapi/page/cpdf_dib.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_generalstate.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_meshstream.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_pattern.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_shadingpattern.h"
#include "core/fpdfapi/page/cpdf_streamparser.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_tilingpattern.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_syntax_parser.h"
#include "core/fpdftext/cpdf_textpage.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxge/dib/cfx_dibbase.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "inkbar/redact.h"
#include "inkbar/sanitize.h"

namespace {

constexpr char kOverlayKey[] = "INKB_Overlay";
constexpr char kPageCopyKey[] = "INKB_PageCopy";

// Paths and shadings are tested against each region shrunk by this much, in
// points. The apply cuts them a hair outside the region (its boundary guard),
// and the save rounds coordinates, so a correctly cut edge must not count.
constexpr float kPathInset = 0.01f;
// The second, independent tests for text and paths use regions shrunk by this
// much on every side, or by a quarter of the smaller side when that is less.
constexpr float kSecondInset = 1.0f;
// An image is blank when every stored sample the apply would blank is within
// this many levels of one value, per 8-bit channel: 0 when the image's last
// filter is lossless, this many either way after DCT or JPX.
constexpr int kLossyTolerance = 12;
// In a JPEG, samples in a block (8 x 8, or the MCU when chroma is
// subsampled) that the edge of the blanked area crosses may stray twice as
// far: the apply re-encodes those blocks with the kept content beside the
// hole, and that content rings into it. Every other sample is held to the
// tolerance above. A JPEG whose block size cannot be read is taken to have
// this one, the largest baseline MCU.
constexpr int kJpegLargestBlock = 16;
// Filled boxes that count as candidates in the page content.
constexpr float kBoxMinSize = 4.0f;
constexpr float kBoxDarkBelow = 0.2f;
constexpr float kBoxLightAbove = 0.98f;
// PDFium's content parser leaves a form nested deeper than this unparsed and
// empty (kMaxFormLevel in cpdf_streamcontentparser.cpp), though another
// viewer may draw it. Meeting one fails the inspection.
constexpr int kFormLevelLimit = 40;
// Regions are cut to the page grown by this much on every side.
constexpr float kPageMargin = 10.0f;
// A ceiling on the steps of one sampling loop; a page can be as large as its
// MediaBox says. A length that needs more cannot be sampled a point apart,
// and what is sampled along it counts as present.
constexpr double kMaxSteps = 1 << 20;
// A ceiling on the points sampled for one object under one region. Meeting it
// counts the object as present: the check fails closed.
constexpr double kMaxSamples = 1 << 22;
// A shading's curved clips are flattened to within this many points, each
// curve split at most kMaxCurveDepth times and each clip into at most
// kMaxClipPoints points. Meeting either limit, or a coordinate that is not
// finite, counts the shading as painting the region.
constexpr float kCurveTolerance = 0.25f;
constexpr int kMaxCurveDepth = 8;
constexpr size_t kMaxClipPoints = 1 << 16;
// Patterns painted with patterns, more deeply nested than this, fail the
// inspection rather than going unread.
constexpr int kMaxPatternChain = 64;
// What the sampling tests of one INK_Inspect_Page call may do, over every
// object and region: one unit for each point they try, each clip edge they
// test against a row of points, and each point they flatten or read. About a
// nanosecond a unit, so a call stops within a few seconds. Past it the call
// fails rather than report what it did not look at.
constexpr uint64_t kWorkBudget = uint64_t{1} << 30;

struct Work {
  // Takes |units| from the budget. False once the budget has run out.
  bool Spend(uint64_t units) {
    if (spent || units > left) {
      spent = true;
      return false;
    }
    left -= units;
    return true;
  }

  uint64_t left = kWorkBudget;
  bool spent = false;
};

// -- Geometry, as cpdf_text_redactor.cpp tests glyphs ------------------------

using Quad = std::array<CFX_PointF, 4>;  // /QuadPoints slot order.

constexpr int kQuadRing[4] = {0, 1, 3, 2};

bool Intersects(const CFX_FloatRect& a, const CFX_FloatRect& b) {
  return a.right > b.left && a.left < b.right && a.top > b.bottom &&
         a.bottom < b.top;
}

Quad QuadFromRect(const CFX_FloatRect& r) {
  return {CFX_PointF(r.left, r.top), CFX_PointF(r.right, r.top),
          CFX_PointF(r.left, r.bottom), CFX_PointF(r.right, r.bottom)};
}

CFX_FloatRect BBoxOf(const Quad& q) {
  CFX_FloatRect box(q[0]);
  for (size_t i = 1; i < q.size(); ++i) {
    box.UpdateRect(q[i]);
  }
  return box;
}

bool Separated(const Quad& a, const Quad& b, const CFX_PointF& axis) {
  if (axis.x == 0 && axis.y == 0) {
    return false;
  }
  float a_min = 0, a_max = 0, b_min = 0, b_max = 0;
  for (int i = 0; i < 4; ++i) {
    const float pa = a[i].x * axis.x + a[i].y * axis.y;
    const float pb = b[i].x * axis.x + b[i].y * axis.y;
    a_min = i ? std::min(a_min, pa) : pa;
    a_max = i ? std::max(a_max, pa) : pa;
    b_min = i ? std::min(b_min, pb) : pb;
    b_max = i ? std::max(b_max, pb) : pb;
  }
  return a_max <= b_min || b_max <= a_min;
}

bool HasSeparatingAxis(const Quad& a, const Quad& b) {
  for (int i = 0; i < 4; ++i) {
    const CFX_PointF& p = a[kQuadRing[i]];
    const CFX_PointF& q = a[kQuadRing[(i + 1) % 4]];
    if (Separated(a, b, CFX_PointF(-(q.y - p.y), q.x - p.x))) {
      return true;
    }
  }
  return false;
}

// Whether |q| meets a region's interior.
bool QuadMeets(const Quad& q, const RedactRegion& region) {
  if (!Intersects(BBoxOf(q), region.bbox)) {
    return false;
  }
  const Quad r = region.has_quad ? region.quad : QuadFromRect(region.bbox);
  return !HasSeparatingAxis(q, r) && !HasSeparatingAxis(r, q);
}

bool QuadMeetsAny(const Quad& q, const std::vector<RedactRegion>& regions) {
  for (const RedactRegion& region : regions) {
    if (QuadMeets(q, region)) {
      return true;
    }
  }
  return false;
}

bool RectMeetsAnyBBox(const CFX_FloatRect& rect,
                      const std::vector<RedactRegion>& regions) {
  for (const RedactRegion& region : regions) {
    if (Intersects(rect, region.bbox)) {
      return true;
    }
  }
  return false;
}

// Steps of at most 1 pt over |length|, at least one. Zero when |length| needs
// more than kMaxSteps or is not finite: it cannot be sampled.
int Steps(double length) {
  if (!std::isfinite(length) || length > kMaxSteps) {
    return 0;
  }
  if (!(length > 1)) {
    return 1;
  }
  return static_cast<int>(std::ceil(length));
}

// |region| shrunk by |amount| on every side, or nullopt when nothing is
// left. A quad's edges each move |amount| inward, perpendicular to
// themselves, and its new corners are where the moved edges meet.
std::optional<RedactRegion> Inset(const RedactRegion& region, float amount) {
  if (!region.has_quad) {
    CFX_FloatRect box = region.bbox;
    box.Deflate(amount, amount);
    if (!(box.right > box.left) || !(box.top > box.bottom)) {
      return std::nullopt;
    }
    RedactRegion inset;
    inset.bbox = box;
    return inset;
  }
  // The corners in ring order, and the ring's winding.
  std::array<CFX_PointF, 4> ring;
  for (int i = 0; i < 4; ++i) {
    ring[i] = region.quad[kQuadRing[i]];
  }
  float area = 0;
  for (int i = 0; i < 4; ++i) {
    const CFX_PointF& p = ring[i];
    const CFX_PointF& q = ring[(i + 1) % 4];
    area += p.x * q.y - q.x * p.y;
  }
  if (area == 0) {
    return std::nullopt;
  }
  const float inward = area > 0 ? 1.0f : -1.0f;
  // Edge i runs from ring[i] to ring[i + 1], moved inward.
  std::array<CFX_PointF, 4> origin;
  std::array<CFX_PointF, 4> direction;
  for (int i = 0; i < 4; ++i) {
    const CFX_PointF d = ring[(i + 1) % 4] - ring[i];
    const float length = std::hypot(d.x, d.y);
    if (!(length > 0)) {
      return std::nullopt;
    }
    const CFX_PointF normal(-d.y / length * inward, d.x / length * inward);
    origin[i] = CFX_PointF(ring[i].x + normal.x * amount,
                           ring[i].y + normal.y * amount);
    direction[i] = d;
  }
  // Corner ring[i + 1] is where moved edges i and i + 1 meet.
  std::array<CFX_PointF, 4> moved;
  for (int i = 0; i < 4; ++i) {
    const int j = (i + 1) % 4;
    const CFX_PointF& a = direction[i];
    const CFX_PointF& b = direction[j];
    const float cross = a.x * b.y - a.y * b.x;
    if (cross == 0) {
      return std::nullopt;
    }
    const CFX_PointF w = origin[j] - origin[i];
    const float t = (w.x * b.y - w.y * b.x) / cross;
    moved[j] = CFX_PointF(origin[i].x + a.x * t, origin[i].y + a.y * t);
  }
  // Nothing is left when an edge turned round.
  for (int i = 0; i < 4; ++i) {
    const CFX_PointF d = moved[(i + 1) % 4] - moved[i];
    if (d.x * direction[i].x + d.y * direction[i].y <= 0) {
      return std::nullopt;
    }
  }
  Quad corners;
  for (int i = 0; i < 4; ++i) {
    corners[kQuadRing[i]] = moved[i];
  }
  return RedactRegionFromQuadCorners(corners);
}

// The inset of the second, independent tests: 1 pt, or a quarter of the
// region's smaller side when that is less.
float SecondTestInset(const RedactRegion& region) {
  float side = std::min(region.bbox.Width(), region.bbox.Height());
  if (region.has_quad) {
    const Quad& q = region.quad;
    side = std::min(std::hypot(q[1].x - q[0].x, q[1].y - q[0].y),
                    std::hypot(q[2].x - q[0].x, q[2].y - q[0].y));
  }
  return std::min(kSecondInset, side / 4);
}

// Whether |point| lies strictly inside |region|.
bool PointInside(const CFX_PointF& point, const RedactRegion& region) {
  const CFX_FloatRect& b = region.bbox;
  if (!(point.x > b.left && point.x < b.right && point.y > b.bottom &&
        point.y < b.top)) {
    return false;
  }
  if (!region.has_quad) {
    return true;
  }
  float sign = 0;
  for (int i = 0; i < 4; ++i) {
    const CFX_PointF& p = region.quad[kQuadRing[i]];
    const CFX_PointF& q = region.quad[kQuadRing[(i + 1) % 4]];
    const float cross =
        (q.x - p.x) * (point.y - p.y) - (q.y - p.y) * (point.x - p.x);
    if (cross == 0 || (sign != 0 && (cross > 0) != (sign > 0))) {
      return false;
    }
    sign = cross;
  }
  return true;
}

bool PointInsideAny(const CFX_PointF& point,
                    const std::vector<RedactRegion>& regions) {
  for (const RedactRegion& region : regions) {
    if (PointInside(point, region)) {
      return true;
    }
  }
  return false;
}

// -- Images, as cpdf_image_redactor.cpp maps regions to samples --------------

struct GridBox {
  int x0, y0, x1, y1;  // Rows count from the bottom; x1 and y1 one past.
};

std::vector<GridBox> GridBoxes(const CFX_Matrix& image_to_page,
                               int width,
                               int height,
                               const std::vector<RedactRegion>& regions) {
  std::vector<GridBox> boxes;
  if (width <= 0 || height <= 0) {
    return boxes;
  }
  const CFX_Matrix page_to_unit = image_to_page.GetInverse();
  for (const RedactRegion& region : regions) {
    CFX_FloatRect unit = page_to_unit.TransformRect(region.bbox);
    unit.Normalize();
    CFX_FloatRect grid(unit.left * width, unit.bottom * height,
                       unit.right * width, unit.top * height);
    grid.Normalize();
    grid.left = std::clamp(grid.left, 0.0f, static_cast<float>(width));
    grid.right = std::clamp(grid.right, 0.0f, static_cast<float>(width));
    grid.bottom = std::clamp(grid.bottom, 0.0f, static_cast<float>(height));
    grid.top = std::clamp(grid.top, 0.0f, static_cast<float>(height));
    if (!(grid.right > grid.left && grid.top > grid.bottom)) {
      continue;
    }
    GridBox b;
    b.x0 = std::clamp(static_cast<int>(std::floor(grid.left)), 0, width);
    b.x1 = std::clamp(static_cast<int>(std::ceil(grid.right)), 0, width);
    b.y0 = std::clamp(static_cast<int>(std::floor(grid.bottom)), 0, height);
    b.y1 = std::clamp(static_cast<int>(std::ceil(grid.top)), 0, height);
    if (b.x1 > b.x0 && b.y1 > b.y0) {
      boxes.push_back(b);
    }
  }
  return boxes;
}

// The last filter of an image, with the abbreviations of inline images.
ByteString LastFilter(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Object> filter = dict->GetDirectObjectFor("Filter");
  if (!filter) {
    filter = dict->GetDirectObjectFor("F");
  }
  if (!filter) {
    return ByteString();
  }
  if (const CPDF_Array* array = filter->AsArray()) {
    return array->IsEmpty() ? ByteString()
                            : array->GetByteStringAt(array->size() - 1);
  }
  return filter->GetString();
}

int Tolerance(const CPDF_Dictionary* dict) {
  const ByteString filter = LastFilter(dict);
  return filter == "DCTDecode" || filter == "DCT" || filter == "JPXDecode"
             ? kLossyTolerance
             : 0;
}

// Tracks whether samples stay within a band around one value.
class Band {
 public:
  explicit Band(int tolerance) : tolerance_(tolerance) {}
  void Add(int channel, int value) {
    if (!seen_[channel]) {
      seen_[channel] = true;
      min_[channel] = max_[channel] = value;
      return;
    }
    min_[channel] = std::min(min_[channel], value);
    max_[channel] = std::max(max_[channel], value);
    if (max_[channel] - min_[channel] > 2 * tolerance_) {
      flat_ = false;
    }
  }
  bool flat() const { return flat_; }

 private:
  const int tolerance_;
  bool flat_ = true;
  bool seen_[3] = {false, false, false};
  int min_[3] = {0, 0, 0};
  int max_[3] = {0, 0, 0};
};

// An image's or a mask's samples, decoded once per inspection: BGR (three or
// four bytes a pixel), or a mask format (1 or 8 bits a sample).
struct Samples {
  RetainPtr<const CFX_DIBBase> dib;  // Null when it could not be decoded.
  int tolerance = 0;
  // A JPEG's block grid, from its top left; 0 for any other image.
  int block_width = 0;
  int block_height = 0;
};

// The size of a JPEG's coding blocks (its MCU) from its frame header: 8 x 8
// for one component, else 8 times the largest sampling factors. Nullopt when
// there is no frame header to read.
std::optional<std::pair<int, int>> JpegBlock(pdfium::span<const uint8_t> data) {
  size_t at = 2;
  if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
    return std::nullopt;
  }
  while (at + 4 <= data.size()) {
    if (data[at] != 0xFF) {
      return std::nullopt;
    }
    const uint8_t marker = data[at + 1];
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    const size_t length = (data[at + 2] << 8) | data[at + 3];
    const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                       marker != 0xC8 && marker != 0xCC;
    if (frame) {
      const size_t header = at + 4;
      if (header + 6 > data.size()) {
        return std::nullopt;
      }
      const int components = data[header + 5];
      if (components == 1) {
        return std::make_pair(8, 8);
      }
      int h = 1;
      int v = 1;
      for (int c = 0; c < components; ++c) {
        const size_t factors = header + 6 + c * 3 + 1;
        if (factors >= data.size()) {
          return std::nullopt;
        }
        h = std::max(h, data[factors] >> 4);
        v = std::max(v, data[factors] & 0x0F);
      }
      return std::make_pair(8 * h, 8 * v);
    }
    if (marker == 0xDA || length < 2) {
      return std::nullopt;
    }
    at += 2 + length;
  }
  return std::nullopt;
}

// Decoded samples kept for one inspection are released, least recently used
// first, once they pass this many bytes.
constexpr size_t kSampleCacheBytes = 64 * 1024 * 1024;

// One stream's samples, with the value the apply fills a hole in them with,
// decoded the same way: a value per channel (B, G, R), or one for a mask.
// Empty when that value is not known.
struct DecodedImage {
  Samples samples;
  std::vector<int> fill;
};

class SampleCache {
 public:
  SampleCache(CPDF_Document* doc, const CPDF_Dictionary* page_resources)
      : doc_(doc), page_resources_(page_resources) {}

  DecodedImage Get(RetainPtr<const CPDF_Stream> stream,
                   RedactionHoleRole role) {
    const Key key{stream.Get(), role};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      order_.splice(order_.begin(), order_, it->second.place);
      return it->second.image;
    }
    DecodedImage image;
    Samples& samples = image.samples;
    RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
    samples.tolerance = Tolerance(dict.Get());
    const ByteString filter = LastFilter(dict.Get());
    if (filter == "DCTDecode" || filter == "DCT") {
      // The JPEG itself: its data through every filter before the DCT.
      auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
      acc->LoadAllDataImageAcc(0);
      const std::pair<int, int> block =
          JpegBlock(acc->GetSpan())
              .value_or(std::make_pair(kJpegLargestBlock, kJpegLargestBlock));
      samples.block_width = block.first;
      samples.block_height = block.second;
    }
    samples.dib = Decode(stream);
    image.fill = Fill(*dict, role);

    const size_t bytes =
        samples.dib ? static_cast<size_t>(samples.dib->GetPitch()) *
                          static_cast<size_t>(samples.dib->GetHeight())
                    : 0;
    order_.push_front(key);
    cache_.emplace(key, Entry{stream, image, bytes, order_.begin()});
    bytes_ += bytes;
    while (bytes_ > kSampleCacheBytes && order_.size() > 1) {
      auto last = cache_.find(order_.back());
      bytes_ -= last->second.bytes;
      cache_.erase(last);
      order_.pop_back();
    }
    return image;
  }

 private:
  // A stream and the part it plays: one stream can be an image's /Mask and
  // also be drawn as a stencil, and the fill differs.
  using Key = std::pair<const CPDF_Stream*, RedactionHoleRole>;

  struct Entry {
    // Held, so its address names it while it is cached.
    RetainPtr<const CPDF_Stream> stream;
    DecodedImage image;
    size_t bytes;
    std::list<Key>::iterator place;
  };

  RetainPtr<const CFX_DIBBase> Decode(RetainPtr<const CPDF_Stream> stream) {
    auto dib = pdfium::MakeRetain<CPDF_DIB>(doc_, std::move(stream));
    if (!dib->Load()) {
      return nullptr;
    }
    if (dib->IsJBigImage()) {
      CPDF_DIB::LoadState state = CPDF_DIB::LoadState::kContinue;
      while (state == CPDF_DIB::LoadState::kContinue) {
        state = dib->ContinueLoadDIBBase(nullptr);
      }
      if (state != CPDF_DIB::LoadState::kSuccess) {
        return nullptr;
      }
    }
    const FXDIB_Format format = dib->GetFormat();
    if (dib->IsMaskFormat() || format == FXDIB_Format::kBgr ||
        format == FXDIB_Format::kBgrx || format == FXDIB_Format::kBgra) {
      return dib->Realize();
    }
    return dib->ConvertTo(FXDIB_Format::kBgr);
  }

  // The apply's fill for |dict|'s samples, from upstream's own computation
  // (RedactionHoleSamples()), decoded as a one-sample image with the same
  // colour space, depth and /Decode.
  std::vector<int> Fill(const CPDF_Dictionary& dict, RedactionHoleRole role) {
    std::optional<std::vector<uint32_t>> hole = RedactionHoleSamples(
        doc_, dict, role, nullptr, page_resources_);
    if (!hole.has_value() || hole->empty()) {
      return {};
    }
    const int bpc = role == RedactionHoleRole::kImage ||
                            role == RedactionHoleRole::kSoftMask
                        ? dict.GetIntegerFor("BitsPerComponent", 8)
                        : 1;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) {
      return {};
    }
    DataVector<uint8_t> data((hole->size() * bpc + 7) / 8);
    size_t bit = 0;
    for (uint32_t value : *hole) {
      if (bpc == 16) {
        data[bit / 8] = static_cast<uint8_t>(value >> 8);
        data[bit / 8 + 1] = static_cast<uint8_t>(value);
      } else {
        const int shift = 8 - bpc - static_cast<int>(bit % 8);
        data[bit / 8] |= static_cast<uint8_t>(value << shift);
      }
      bit += bpc;
    }
    RetainPtr<CPDF_Dictionary> one = ToDictionary(dict.Clone());
    for (const char* key : {"Filter", "DecodeParms", "F", "DP", "SMask", "Mask",
                            "SMaskInData", "Length"}) {
      one->RemoveFor(key);
    }
    one->SetNewFor<CPDF_Number>("Width", 1);
    one->SetNewFor<CPDF_Number>("Height", 1);
    auto stream =
        pdfium::MakeRetain<CPDF_Stream>(std::move(data), std::move(one));
    RetainPtr<const CFX_DIBBase> dib = Decode(std::move(stream));
    if (!dib || dib->GetWidth() < 1 || dib->GetHeight() < 1) {
      return {};
    }
    pdfium::span<const uint8_t> row = dib->GetScanline(0);
    if (dib->IsMaskFormat()) {
      if (row.empty()) {
        return {};
      }
      return {dib->GetBPP() == 1 ? row[0] >> 7 : row[0]};
    }
    if (row.size() < 3) {
      return {};
    }
    return {row[0], row[1], row[2]};
  }

  CPDF_Document* const doc_;
  const CPDF_Dictionary* const page_resources_;
  std::map<Key, Entry> cache_;
  std::list<Key> order_;  // Most recently used first.
  size_t bytes_ = 0;
};

// Whether the samples of |samples| over the regions, on its own grid, are
// one value. Samples that could not be decoded are not.
//
// With |fill| (regions given), each sample must instead be within the
// tolerance of the apply's fill value: a flat image that is not the fill is
// content. An empty |fill| when one is wanted fails.
bool SamplesFlat(const Samples& samples,
                 const CFX_Matrix& image_to_page,
                 const std::vector<RedactRegion>& regions,
                 const std::vector<int>* fill) {
  if (fill && fill->empty()) {
    return false;
  }
  const CFX_DIBBase* dib = samples.dib.Get();
  if (!dib) {
    return false;
  }
  const int width = dib->GetWidth();
  const int height = dib->GetHeight();
  const std::vector<GridBox> boxes =
      GridBoxes(image_to_page, width, height, regions);
  // |band| holds every sample; |inner| those outside the JPEG blocks the
  // edge of a box crosses.
  const bool jpeg = samples.block_width > 0 && samples.block_height > 0;
  Band band(jpeg ? 2 * samples.tolerance : samples.tolerance);
  Band inner(samples.tolerance);
  // Whether the block holding sample (x, row from the top) is crossed by the
  // edge of |b|: it meets the box without lying wholly inside it.
  auto crossed = [&](const GridBox& b, int x, int top_row) {
    const int bx0 = x / samples.block_width * samples.block_width;
    const int by0 = top_row / samples.block_height * samples.block_height;
    const int bx1 = std::min(bx0 + samples.block_width, width);
    const int by1 = std::min(by0 + samples.block_height, height);
    const int box_top = height - b.y1;
    const int box_bottom = height - b.y0;
    return bx0 < b.x0 || bx1 > b.x1 || by0 < box_top || by1 > box_bottom;
  };
  const bool mask = dib->IsMaskFormat();
  if (fill && fill->size() != (mask ? 1u : 3u)) {
    return false;
  }
  bool on_fill = true;
  auto add = [&](int channel, int value, bool edge) {
    if (fill) {
      // Within the tolerance of the fill; in a crossed JPEG block, anywhere
      // in a band of the looser range that holds the fill.
      const int tolerance = edge ? 4 * samples.tolerance : samples.tolerance;
      on_fill &= std::abs(value - (*fill)[channel]) <= tolerance;
      return;
    }
    band.Add(channel, value);
    if (!edge) {
      inner.Add(channel, value);
    }
  };
  const bool one_bit = dib->GetBPP() == 1;
  const size_t bytes = mask ? 1 : dib->GetBPP() / 8;
  for (const GridBox& b : boxes) {
    for (int y = b.y0; y < b.y1 && band.flat() && inner.flat() && on_fill;
         ++y) {
      pdfium::span<const uint8_t> row = dib->GetScanline(height - 1 - y);
      for (int x = b.x0; x < b.x1; ++x) {
        const bool edge = jpeg && crossed(b, x, height - 1 - y);
        if (mask) {
          const size_t at = one_bit ? x / 8 : x;
          if (at >= row.size()) {
            return false;
          }
          add(0, one_bit ? (row[at] >> (7 - x % 8)) & 1 : row[at], edge);
          continue;
        }
        const size_t at = static_cast<size_t>(x) * bytes;
        if (at + 2 >= row.size()) {
          return false;
        }
        for (int c = 0; c < 3; ++c) {
          add(c, row[at + c], edge);
        }
      }
    }
  }
  return band.flat() && inner.flat() && on_fill;
}

// Whether an image is blank under the regions: its own samples, and those of
// its /SMask and of a stream /Mask, each on its own grid.
//
// With |strict| (regions given), blank means the apply's fill value; without,
// one flat value of any kind.
bool ImageBlank(SampleCache* cache,
                const CPDF_ImageObject* object,
                const CFX_Matrix& to_page,
                const std::vector<RedactRegion>& regions,
                bool strict) {
  RetainPtr<CPDF_Image> image = object->GetImage();
  RetainPtr<const CPDF_Stream> stream = image ? image->GetStream() : nullptr;
  if (!stream) {
    return false;
  }
  const CFX_Matrix image_to_page = object->matrix() * to_page;
  auto blank = [&](RetainPtr<const CPDF_Stream> part, RedactionHoleRole role) {
    const DecodedImage decoded = cache->Get(std::move(part), role);
    return SamplesFlat(decoded.samples, image_to_page, regions,
                       strict ? &decoded.fill : nullptr);
  };
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  if (!blank(stream, dict->GetBooleanFor("ImageMask", false)
                         ? RedactionHoleRole::kStencil
                         : RedactionHoleRole::kImage)) {
    return false;
  }
  RetainPtr<const CPDF_Stream> soft_mask = dict->GetStreamFor("SMask");
  if (soft_mask && !blank(soft_mask, RedactionHoleRole::kSoftMask)) {
    return false;
  }
  RetainPtr<const CPDF_Stream> mask = dict->GetStreamFor("Mask");
  return !mask || blank(mask, RedactionHoleRole::kMask);
}

// -- Paths -------------------------------------------------------------------

// The largest factor by which |m| stretches a length.
float Stretch(const CFX_Matrix& m) {
  const float sum = m.a * m.a + m.b * m.b + m.c * m.c + m.d * m.d;
  const float difference = m.a * m.a + m.b * m.b - m.c * m.c - m.d * m.d;
  const float cross = m.a * m.c + m.b * m.d;
  return std::sqrt(
      (sum + std::sqrt(difference * difference + 4 * cross * cross)) / 2);
}

// A box that holds everything |path| paints, placed by |to_page| from its
// holder's space: the bounds of its outline, grown for a stroke by half the
// line width, taken without its sign, times the miter limit when the joins
// are mitred (a corner may reach that far) or the square root of 2 for
// square caps, under the largest stretch in effect. A hairline, of width 0,
// is a point wide on the page. PDFium's own box for a path is not used: a
// negative width pulls it inward, off the fill, and it leaves out the miter
// where a closed outline starts.
CFX_FloatRect PathPaintBox(const CPDF_PathObject& path,
                           const CFX_Matrix& to_page) {
  const CFX_Matrix placed = path.matrix() * to_page;
  CFX_FloatRect box = path.path().HasRef()
                          ? placed.TransformRect(path.path().GetBoundingBox())
                          : CFX_FloatRect();
  box.Normalize();
  if (path.stroke()) {
    const CFX_GraphState& state = path.graph_state();
    const float width = std::fabs(state.GetLineWidth());
    float grow = 0.5f;
    if (width > 0) {
      float factor = 1;
      if (state.GetLineJoin() == CFX_GraphStateData::LineJoin::kMiter) {
        factor = std::max(factor, state.GetMiterLimit());
      }
      if (state.GetLineCap() == CFX_GraphStateData::LineCap::kSquare) {
        factor = std::max(factor, 1.4143f);
      }
      grow = width / 2 * factor * Stretch(placed);
    }
    box.Inflate(grow, grow);
  }
  return box;
}

// A copy of |source| to hand to PDFium's path redactor, which cuts what it is
// given: the redactor says whether it would cut the original. The copy's
// box is widened to PathPaintBox(), so the redactor's first look by the box
// does not pass over a path whose own box is too small.
std::unique_ptr<CPDF_PathObject> Probe(const CPDF_PathObject& source) {
  auto probe = std::make_unique<CPDF_PathObject>();
  probe->mutable_graph_state() = source.graph_state();
  probe->mutable_color_state() = source.color_state();
  probe->mutable_general_state() = source.general_state();
  probe->set_stroke(source.stroke());
  probe->set_filltype(source.filltype());
  probe->path() = source.path();
  probe->SetPathMatrix(source.matrix());
  probe->CalcBoundingBox();
  CFX_FloatRect rect = probe->GetRect();
  rect.Normalize();
  rect.Union(PathPaintBox(source, CFX_Matrix()));
  probe->SetRect(rect);
  return probe;
}

// A filled path to probe the area |path| covers.
std::unique_ptr<CPDF_PathObject> Probe(const CPDF_Path& path,
                                       CFX_FillRenderOptions::FillType fill) {
  auto probe = std::make_unique<CPDF_PathObject>();
  probe->path() = path;
  probe->set_filltype(fill);
  probe->CalcBoundingBox();
  return probe;
}

// Whether the redactor would cut |path|. A path it cannot read counts.
bool PathCrosses(const CPDF_PathRedactor& redactor,
                 const CPDF_PathObject& path,
                 const CFX_Matrix& to_page) {
  const PathRedactionResult result =
      redactor.Redact(Probe(path).get(), to_page);
  return !result.succeeded || result.changed;
}

// Whether a shading paints inside the regions: its box crosses them and so
// does each of its clip paths. The redactor clips a shading it cuts with the
// area outside the region, so a cut shading does not count.
bool ShadingCrosses(const CPDF_PathRedactor& redactor,
                    const CPDF_ShadingObject& shading,
                    const CFX_Matrix& to_page) {
  CPDF_Path box;
  box.Emplace();
  box.AppendFloatRect(shading.GetRect());
  PathRedactionResult result = redactor.Redact(
      Probe(box, CFX_FillRenderOptions::FillType::kWinding).get(), to_page);
  if (result.succeeded && !result.changed) {
    return false;
  }
  const CPDF_ClipPath& clip = shading.clip_path();
  if (!clip.HasRef()) {
    return true;
  }
  for (size_t i = 0; i < clip.GetPathCount(); ++i) {
    result = redactor.Redact(Probe(clip.GetPath(i), clip.GetClipType(i)).get(),
                             to_page);
    if (result.succeeded && !result.changed) {
      return false;
    }
  }
  return true;
}

bool Finite(const CFX_PointF& p) {
  return std::isfinite(p.x) && std::isfinite(p.y);
}

// Appends the cubic |p0| .. |p3| to |polygon| as lines, split until every
// line lies within |tolerance| of the curve. False when that needs more than
// kMaxCurveDepth splits or kMaxClipPoints points, or meets a coordinate that
// is not finite.
bool FlattenCubic(const CFX_PointF& p0,
                  const CFX_PointF& p1,
                  const CFX_PointF& p2,
                  const CFX_PointF& p3,
                  float tolerance,
                  int depth,
                  std::vector<CFX_PointF>* polygon) {
  if (!Finite(p0) || !Finite(p1) || !Finite(p2) || !Finite(p3)) {
    return false;
  }
  // The control points' distance from the chord bounds the curve's.
  const CFX_PointF chord = p3 - p0;
  const float length = std::hypot(chord.x, chord.y);
  auto away = [&](const CFX_PointF& q) {
    if (length == 0) {
      return std::hypot(q.x - p0.x, q.y - p0.y);
    }
    return std::fabs(chord.x * (q.y - p0.y) - chord.y * (q.x - p0.x)) /
           length;
  };
  const float d1 = away(p1);
  const float d2 = away(p2);
  if (!std::isfinite(d1) || !std::isfinite(d2)) {
    return false;
  }
  if (d1 <= tolerance && d2 <= tolerance) {
    if (polygon->size() >= kMaxClipPoints) {
      return false;
    }
    polygon->push_back(p3);
    return true;
  }
  if (depth >= kMaxCurveDepth) {
    return false;
  }
  const CFX_PointF a((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
  const CFX_PointF b((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
  const CFX_PointF c((p2.x + p3.x) / 2, (p2.y + p3.y) / 2);
  const CFX_PointF ab((a.x + b.x) / 2, (a.y + b.y) / 2);
  const CFX_PointF bc((b.x + c.x) / 2, (b.y + c.y) / 2);
  const CFX_PointF mid((ab.x + bc.x) / 2, (ab.y + bc.y) / 2);
  return FlattenCubic(p0, a, ab, mid, tolerance, depth + 1, polygon) &&
         FlattenCubic(mid, bc, c, p3, tolerance, depth + 1, polygon);
}

// The subpaths of |path| as closed polygons, in the path's own space, its
// curves cut into lines within |tolerance| of them. Nullopt when a limit of
// FlattenCubic() is met.
std::optional<std::vector<std::vector<CFX_PointF>>> Polygons(
    const CPDF_Path& path,
    float tolerance) {
  std::vector<std::vector<CFX_PointF>> polygons;
  size_t total = 0;
  const std::vector<CFX_Path::Point>& points = path.GetPoints();
  for (size_t i = 0; i < points.size(); ++i) {
    const CFX_Path::Point& point = points[i];
    if (!Finite(point.point_)) {
      return std::nullopt;
    }
    if (point.type_ == CFX_Path::Point::Type::kMove || polygons.empty()) {
      polygons.emplace_back();
      polygons.back().push_back(point.point_);
    } else if (point.type_ == CFX_Path::Point::Type::kLine) {
      polygons.back().push_back(point.point_);
    } else {
      if (i + 2 >= points.size()) {
        break;
      }
      std::vector<CFX_PointF>& polygon = polygons.back();
      const size_t before = polygon.size();
      if (!FlattenCubic(polygon.back(), point.point_, points[i + 1].point_,
                        points[i + 2].point_, tolerance, 0, &polygon)) {
        return std::nullopt;
      }
      total += polygon.size() - before;
      i += 2;
    }
    if (++total > kMaxClipPoints) {
      return std::nullopt;
    }
  }
  return polygons;
}

bool Finite(const CFX_FloatRect& r) {
  return std::isfinite(r.left) && std::isfinite(r.bottom) &&
         std::isfinite(r.right) && std::isfinite(r.top);
}

bool Finite(const CFX_Matrix& m) {
  return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) &&
         std::isfinite(m.d) && std::isfinite(m.e) && std::isfinite(m.f);
}

// A clip of a shading in page space, as the edges a row of points can cross.
struct ClipEdges {
  struct Edge {
    CFX_PointF a;
    CFX_PointF b;
  };

  // Adds the closed |polygon|, given in the holder's space. False when a
  // point of it is not finite on the page.
  bool Add(const std::vector<CFX_PointF>& polygon, const CFX_Matrix& to_page) {
    std::vector<CFX_PointF> points;
    points.reserve(polygon.size());
    for (const CFX_PointF& point : polygon) {
      points.push_back(to_page.Transform(point));
      if (!Finite(points.back())) {
        return false;
      }
    }
    for (size_t i = 0; i < points.size(); ++i) {
      const CFX_PointF& a = points[i];
      const CFX_PointF& b = points[(i + 1) % points.size()];
      if (edges.empty()) {
        bottom = top = a.y;
      }
      bottom = std::min({bottom, a.y, b.y});
      top = std::max({top, a.y, b.y});
      edges.push_back({a, b});
    }
    return true;
  }

  std::vector<Edge> edges;
  CFX_FillRenderOptions::FillType fill =
      CFX_FillRenderOptions::FillType::kWinding;
  // No point below |bottom| or above |top| is covered.
  float bottom = 0;
  float top = 0;
};

// -- Shadings that can hold a picture ----------------------------------------
//
// An axial or radial shading is a colour ramp, and the part of it an apply
// leaves in view shows all of it. A mesh (types 4 to 7) or a function over a
// plane (type 1) can hold any picture, and the apply only clips a shading or
// cuts the path it fills: the definition stays in the file.

struct OwnBox {
  // False when the shading's geometry cannot be read.
  bool known = false;
  // Outside this, in the shading's own space, it paints nothing.
  CFX_FloatRect box;
};

// The bounds of a mesh's points: its vertices, or its patches' control
// points, which hold the patches. Nullopt when the mesh cannot be read to
// its end or |work| runs out.
std::optional<CFX_FloatRect> MeshBounds(const CPDF_ShadingPattern& pattern,
                                        Work* work) {
  const ShadingType type = pattern.GetShadingType();
  RetainPtr<const CPDF_Stream> data = ToStream(pattern.GetShadingObject());
  RetainPtr<CPDF_ColorSpace> colours = pattern.GetCS();
  if (!data || !colours) {
    return std::nullopt;
  }
  CPDF_MeshStream stream(type, pattern.GetFuncs(), std::move(data),
                         std::move(colours));
  if (!stream.Load()) {
    return std::nullopt;
  }
  const bool patches = type == kCoonsPatchMeshShading ||
                       type == kTensorProductPatchMeshShading;
  const bool lattice = type == kLatticeFormGouraudTriangleMeshShading;
  std::optional<CFX_FloatRect> bounds;
  while (!stream.IsEOF()) {
    uint32_t flag = 0;
    if (!lattice) {
      if (!stream.CanReadFlag()) {
        break;
      }
      flag = stream.ReadFlag();
    }
    // A patch that shares an edge with the one before brings four points
    // and two colours fewer.
    int points = 1;
    uint32_t colour_count = 1;
    if (patches) {
      points = type == kTensorProductPatchMeshShading ? 16 : 12;
      colour_count = 4;
      if (flag) {
        points -= 4;
        colour_count -= 2;
      }
    }
    if (!work->Spend(points)) {
      return std::nullopt;
    }
    for (int i = 0; i < points; ++i) {
      if (!stream.CanReadCoords()) {
        return std::nullopt;
      }
      const CFX_PointF point = stream.ReadCoords();
      if (!Finite(point)) {
        return std::nullopt;
      }
      if (bounds.has_value()) {
        bounds->UpdateRect(point);
      } else {
        bounds = CFX_FloatRect(point);
      }
    }
    const uint64_t bits = static_cast<uint64_t>(stream.Components()) *
                          stream.ComponentBits() * colour_count;
    if (bits > UINT32_MAX) {
      return std::nullopt;
    }
    stream.SkipBits(static_cast<uint32_t>(bits));
    if (!patches) {
      stream.ByteAlign();
    }
  }
  // A mesh with no points paints nothing.
  return bounds.value_or(CFX_FloatRect());
}

// Where a shading of type 1 or 4 to 7 can paint, clips aside: its /BBox,
// and its mesh's bounds or its domain under its matrix.
OwnBox ShadingOwnBox(const CPDF_ShadingPattern& pattern, Work* work) {
  RetainPtr<const CPDF_Object> object = pattern.GetShadingObject();
  RetainPtr<const CPDF_Dictionary> dict = object ? object->GetDict() : nullptr;
  if (!dict) {
    return {};
  }
  std::optional<CFX_FloatRect> bounds;
  if (pattern.IsMeshShading()) {
    bounds = MeshBounds(pattern, work);
  } else if (pattern.GetShadingType() == kFunctionBasedShading) {
    CFX_FloatRect domain(0, 0, 1, 1);
    RetainPtr<const CPDF_Array> array = dict->GetArrayFor("Domain");
    if (array && array->size() >= 4) {
      domain = CFX_FloatRect(array->GetFloatAt(0), array->GetFloatAt(2),
                             array->GetFloatAt(1), array->GetFloatAt(3));
      domain.Normalize();
    }
    bounds = dict->GetMatrixFor("Matrix").TransformRect(domain);
  }
  std::optional<CFX_FloatRect> box;
  if (dict->KeyExist("BBox")) {
    // Not four numbers: another reader ignores it and paints.
    RetainPtr<const CPDF_Array> array = dict->GetArrayFor("BBox");
    if (!array || array->size() != 4) {
      return {};
    }
    for (size_t i = 0; i < 4; ++i) {
      RetainPtr<const CPDF_Object> number = array->GetDirectObjectAt(i);
      if (!number || !number->IsNumber()) {
        return {};
      }
    }
    box = dict->GetRectFor("BBox");
    box->Normalize();
  }
  if (bounds.has_value() && box.has_value()) {
    box->Intersect(*bounds);
  } else if (bounds.has_value()) {
    box = bounds;
  }
  if (!box.has_value() || !Finite(*box)) {
    return {};
  }
  return {true, *box};
}

// Whether |function| is an exponential function (type 2): one colour run.
bool Exponential(const CPDF_Object* function) {
  RetainPtr<const CPDF_Dictionary> dict = function ? function->GetDict() : nullptr;
  return dict && dict->GetIntegerFor("FunctionType") == 2;
}

// Whether |function| draws a plain ramp: an exponential function, or a
// stitch of at most 8 of them.
bool PlainFunction(const CPDF_Object* function) {
  if (Exponential(function)) {
    return true;
  }
  RetainPtr<const CPDF_Dictionary> dict = function ? function->GetDict() : nullptr;
  if (!dict || dict->GetIntegerFor("FunctionType") != 3) {
    return false;
  }
  RetainPtr<const CPDF_Array> pieces = dict->GetArrayFor("Functions");
  if (!pieces || pieces->size() == 0 || pieces->size() > 8) {
    return false;
  }
  for (size_t i = 0; i < pieces->size(); ++i) {
    if (!Exponential(pieces->GetDirectObjectAt(i).Get())) {
      return false;
    }
  }
  return true;
}

// Whether a shading is a plain colour ramp, which the part an apply leaves
// in view shows all of: axial or radial, its function plain. A sampled or
// calculated function, or a long stitch, can hold a picture along the ramp.
bool Ramp(const CPDF_ShadingPattern* pattern) {
  if (!pattern || (pattern->GetShadingType() != kAxialShading &&
                   pattern->GetShadingType() != kRadialShading)) {
    return false;
  }
  RetainPtr<const CPDF_Object> object = pattern->GetShadingObject();
  RetainPtr<const CPDF_Dictionary> dict = object ? object->GetDict() : nullptr;
  RetainPtr<const CPDF_Object> function =
      dict ? dict->GetDirectObjectFor("Function") : nullptr;
  if (!function) {
    return false;
  }
  if (const CPDF_Array* functions = function->AsArray()) {
    for (size_t i = 0; i < functions->size(); ++i) {
      if (!PlainFunction(functions->GetDirectObjectAt(i).Get())) {
        return false;
      }
    }
    return functions->size() > 0;
  }
  return PlainFunction(function.Get());
}

// Charges the parsing of |stream| to |work| before it is parsed: a unit a
// byte of its decoded length. Sets |fell_back| when PDFium could not decode
// the stream and hands back the bytes it holds instead: its filters left
// every byte as it was. A stream of 16 bytes or fewer is not asked; an empty
// stream compressed is that small, and nothing can be drawn in so little.
// A parse that then finds no object did not read the stream (see
// Unreadable()). False for no stream.
bool ChargeStream(Work* work, const CPDF_Stream* stream, bool* fell_back) {
  *fell_back = false;
  if (!stream) {
    return false;
  }
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  const size_t size = acc->GetSize();
  work->Spend(size + 1);
  const size_t raw = stream->GetRawSize();
  if (raw <= 16 || !stream->GetDict()->KeyExist("Filter") || size != raw) {
    return true;
  }
  if (!stream->IsMemoryBased()) {
    *fell_back = true;
    return true;
  }
  pdfium::span<const uint8_t> stored = stream->GetInMemoryRawData();
  pdfium::span<const uint8_t> decoded = acc->GetSpan();
  *fell_back = std::equal(decoded.begin(), decoded.end(), stored.begin(),
                          stored.end());
  return true;
}

// Whether every resource the content |bytes| names is found in |resources|:
// an XObject it draws, a shading it paints, a graphics state, a font, a
// pattern or a colour space it sets. PDFium drops an operator whose name it
// cannot find, and the content then parses to less than it draws. The scan
// ends at an inline image, whose data is not content.
bool NamesResolve(pdfium::span<const uint8_t> bytes,
                  const CPDF_Dictionary* resources) {
  CPDF_StreamParser parser(bytes);
  ByteString name;
  bool named = false;
  for (;;) {
    const CPDF_StreamParser::ElementType type = parser.ParseNextElement();
    if (type == CPDF_StreamParser::kEndOfData) {
      return true;
    }
    if (type == CPDF_StreamParser::kName) {
      const ByteStringView word = parser.GetWord();
      name = ByteString(word.Last(word.GetLength() ? word.GetLength() - 1 : 0));
      named = true;
      continue;
    }
    if (type != CPDF_StreamParser::kKeyword) {
      continue;
    }
    const ByteStringView word = parser.GetWord();
    const char* kind = nullptr;
    if (word == "BI") {
      return true;
    } else if (word == "Do") {
      kind = "XObject";
    } else if (word == "sh") {
      kind = "Shading";
    } else if (word == "gs") {
      kind = "ExtGState";
    } else if (word == "Tf") {
      kind = "Font";
    } else if (word == "scn" || word == "SCN") {
      kind = named ? "Pattern" : nullptr;
    } else if (word == "cs" || word == "CS") {
      kind = named && (name == "DeviceGray" || name == "DeviceRGB" ||
                       name == "DeviceCMYK" || name == "Pattern")
                 ? nullptr
                 : "ColorSpace";
    }
    if (kind) {
      RetainPtr<const CPDF_Dictionary> dict =
          resources ? resources->GetDictFor(kind) : nullptr;
      if (!named || !dict || !dict->KeyExist(name.AsStringView())) {
        return false;
      }
    }
    named = false;
  }
}

// Whether a stream that was parsed into |holder| was not read: PDFium could
// not decode it and found no object in the bytes it fell back on. Another
// reader may decode it, or part of it, so it counts as holding content.
bool Unreadable(bool fell_back, const CPDF_PageObjectHolder& holder) {
  return fell_back && holder.GetPageObjectCount() == 0;
}

// The second test for a shading: points sampled at most 1 pt apart inside
// the shrunk regions, each tested against the shading's box and every clip
// path with its fill rule (the nonzero winding rule, or the even-odd rule).
// Whether the shading still paints one of them.
//
// The points are taken a row at a time. Each clip's edges are crossed with
// the row's line once, and the crossings, in order, say which of the row's
// points the clip covers: the cost is rows times edges plus points, taken
// from |work|. True when |work| runs out; the inspection has then failed.
bool ShadingPaintsSample(const CPDF_ShadingObject& shading,
                         const CFX_Matrix& to_page,
                         const std::vector<RedactRegion>& regions,
                         Work* work) {
  CFX_FloatRect box = shading.GetRect();
  box.Normalize();
  CFX_FloatRect page_box = to_page.TransformRect(box);
  page_box.Normalize();
  // A shading whose place on the page is not finite cannot be told from the
  // regions: finite numbers can overflow under the matrix.
  if (!Finite(to_page) || !Finite(page_box)) {
    return true;
  }
  // The part of each region the shading's box can paint; nothing else is
  // sampled, and nothing is flattened when there is none.
  struct Area {
    const RedactRegion* region;
    CFX_FloatRect rect;
    int columns;
    int rows;
  };
  std::vector<Area> areas;
  for (const RedactRegion& region : regions) {
    CFX_FloatRect area = region.bbox;
    area.Intersect(page_box);
    if (area.IsEmpty()) {
      continue;
    }
    const int columns = Steps(area.Width());
    const int rows = Steps(area.Height());
    if (!columns || !rows ||
        static_cast<double>(columns) * rows > kMaxSamples) {
      return true;
    }
    areas.push_back({&region, area, columns, rows});
  }
  if (areas.empty()) {
    return false;
  }
  // A clip whose control points' box misses every area keeps the shading
  // off all of them; only clips that can matter are flattened.
  const CPDF_ClipPath& clip = shading.clip_path();
  std::vector<size_t> matter;
  for (size_t i = 0; clip.HasRef() && i < clip.GetPathCount(); ++i) {
    const CPDF_Path path = clip.GetPath(i);
    const std::vector<CFX_Path::Point>& points = path.GetPoints();
    if (points.empty()) {
      return false;
    }
    if (!work->Spend(points.size())) {
      return true;
    }
    CFX_FloatRect hull(points[0].point_);
    for (const CFX_Path::Point& point : points) {
      if (!Finite(point.point_)) {
        return true;
      }
      hull.UpdateRect(point.point_);
    }
    CFX_FloatRect page_hull = to_page.TransformRect(hull);
    page_hull.Normalize();
    if (!Finite(page_hull)) {
      return true;
    }
    bool meets = false;
    for (const Area& area : areas) {
      meets |= Intersects(page_hull, area.rect);
    }
    if (!meets) {
      return false;
    }
    matter.push_back(i);
  }
  // Curves are flattened to within a quarter point on the page; the
  // matrix's Frobenius norm bounds how far it can stretch a distance.
  const float stretch =
      std::sqrt(to_page.a * to_page.a + to_page.b * to_page.b +
                to_page.c * to_page.c + to_page.d * to_page.d);
  const float tolerance = kCurveTolerance / stretch;
  if (!std::isfinite(tolerance) || !(tolerance > 0)) {
    return true;
  }
  // The shading's box first, as a clip of four edges, then its clip paths.
  std::vector<ClipEdges> clips(1);
  if (!clips[0].Add({CFX_PointF(box.left, box.bottom),
                     CFX_PointF(box.right, box.bottom),
                     CFX_PointF(box.right, box.top),
                     CFX_PointF(box.left, box.top)},
                    to_page)) {
    return true;
  }
  for (size_t i : matter) {
    std::optional<std::vector<std::vector<CFX_PointF>>> polygons =
        Polygons(clip.GetPath(i), tolerance);
    if (!polygons.has_value()) {
      return true;
    }
    ClipEdges entry;
    entry.fill = clip.GetClipType(i);
    for (const std::vector<CFX_PointF>& polygon : *polygons) {
      if (!work->Spend(polygon.size()) || !entry.Add(polygon, to_page)) {
        return true;
      }
    }
    clips.push_back(std::move(entry));
  }

  struct Crossing {
    float x;
    int direction;
  };
  std::vector<Crossing> crossings;
  std::vector<uint8_t> painted;
  for (const Area& area : areas) {
    const auto x_of = [&area](int c) {
      return area.rect.left + (c + 0.5f) * area.rect.Width() / area.columns;
    };
    painted.resize(area.columns);
    for (int r = 0; r < area.rows; ++r) {
      const float y =
          area.rect.bottom + (r + 0.5f) * area.rect.Height() / area.rows;
      if (!work->Spend(area.columns)) {
        return true;
      }
      // The row's points inside the region; each clip then takes away the
      // ones it does not cover, and the first that leaves none ends the row.
      int left = 0;
      for (int c = 0; c < area.columns; ++c) {
        painted[c] = PointInside(CFX_PointF(x_of(c), y), *area.region);
        left += painted[c];
      }
      for (const ClipEdges& entry : clips) {
        if (!left) {
          break;
        }
        if (entry.edges.empty() || y < entry.bottom || y > entry.top) {
          left = 0;
          break;
        }
        if (!work->Spend(entry.edges.size() + area.columns)) {
          return true;
        }
        crossings.clear();
        int winding = 0;
        for (const ClipEdges::Edge& edge : entry.edges) {
          const CFX_PointF& a = edge.a;
          const CFX_PointF& b = edge.b;
          if ((a.y <= y) == (b.y <= y)) {
            continue;
          }
          const float x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
          if (!std::isfinite(x)) {
            return true;
          }
          const int direction = b.y > a.y ? 1 : -1;
          crossings.push_back({x, direction});
          winding += direction;
        }
        // Sorting n crossings takes about n log n steps, and a clip has at
        // most 2^16 points.
        if (!work->Spend(crossings.size() * 16)) {
          return true;
        }
        std::sort(crossings.begin(), crossings.end(),
                  [](const Crossing& p, const Crossing& q) { return p.x < q.x; });
        // A point is covered by what the crossings to its right add up to.
        size_t passed = 0;
        left = 0;
        for (int c = 0; c < area.columns; ++c) {
          const float x = x_of(c);
          while (passed < crossings.size() && crossings[passed].x <= x) {
            winding -= crossings[passed].direction;
            ++passed;
          }
          const bool covered =
              entry.fill == CFX_FillRenderOptions::FillType::kEvenOdd
                  ? (crossings.size() - passed) % 2
                  : winding != 0;
          painted[c] = painted[c] && covered;
          left += painted[c];
        }
      }
      if (left) {
        return true;
      }
    }
  }
  return false;
}

// -- Candidates --------------------------------------------------------------

bool FormHas(const CPDF_Form* form, const char* key) {
  RetainPtr<const CPDF_Dictionary> dict = form ? form->GetDict() : nullptr;
  return dict && dict->GetBooleanFor(key, false);
}

// The page-space box of a path drawn as one opaque filled axis-aligned
// rectangle, or nullopt.
std::optional<CFX_FloatRect> OpaqueFilledBox(const CPDF_PathObject& path,
                                             const CFX_Matrix& to_page) {
  if (path.has_no_filltype() || !path.IsActive()) {
    return std::nullopt;
  }
  const CPDF_GeneralState& state = path.general_state();
  if (state.GetFillAlpha() < 1.0f || state.GetBlendType() != BlendMode::kNormal ||
      state.GetSoftMask()) {
    return std::nullopt;
  }
  const CPDF_Color* fill = path.color_state().GetFillColor();
  if (!fill || fill->IsPattern()) {
    return std::nullopt;
  }
  const std::vector<CFX_Path::Point>& points = path.path().GetPoints();
  std::optional<CFX_PointF> opposite = path.path().GetRectOppositeCorner();
  if (points.empty() || !opposite.has_value()) {
    return std::nullopt;
  }
  const CFX_Matrix m = path.matrix() * to_page;
  if (!((m.b == 0 && m.c == 0) || (m.a == 0 && m.d == 0))) {
    return std::nullopt;
  }
  CFX_FloatRect local(points[0].point_.x, points[0].point_.y,
                      opposite->x, opposite->y);
  local.Normalize();
  CFX_FloatRect box = m.TransformRect(local);
  box.Normalize();
  return box;
}

std::optional<float> Luminance(const CPDF_PathObject& path) {
  std::optional<FX_RGB_STRUCT<float>> rgb =
      path.color_state().GetFillColor()->GetRGB();
  if (!rgb.has_value()) {
    return std::nullopt;
  }
  return 0.2126f * rgb->red + 0.7152f * rgb->green + 0.0722f * rgb->blue;
}

// -- The walk ----------------------------------------------------------------

struct Counts {
  unsigned text_chars = 0;
  unsigned images_not_blank = 0;
  unsigned paths = 0;
  unsigned pattern_content = 0;
  unsigned annotation_text = 0;
  unsigned annotation_images = 0;
  // The second text test's counts, merged into the two above at the end.
  unsigned text_chars_second = 0;
  unsigned annotation_text_second = 0;
};

struct Entry {
  const char* kind = "region";
  CFX_FloatRect rect;
  std::vector<RedactRegion> regions;
  // The regions shrunk for the second, independent tests.
  std::vector<RedactRegion> second;
  // The regions shrunk as the path redactor's are.
  std::vector<RedactRegion> inset;
  // What INK_Redact_Unremovable counts.
  unsigned patterns = 0;
  unsigned shadings = 0;
  unsigned mask_groups = 0;
  unsigned glyphs = 0;
  // Only content painted before this position counts (PageIndex order):
  // everything for a region, what lies under it for a candidate.
  size_t before = SIZE_MAX;
  // A candidate's own drawing, which is not content under it.
  const CPDF_PageObject* own_object = nullptr;
  const CPDF_Dictionary* own_annot = nullptr;
  Counts counts;
};

// The paint order of the page content, forms taken at their place, and which
// objects lie inside a form tagged as an overlay. Annotations paint after the
// content, in /Annots order, from position |content_count| on.
struct PageIndex {
  std::map<const CPDF_PageObject*, size_t> order;
  std::set<const CPDF_PageObject*> in_overlay;
  size_t content_count = 0;
};

struct Context {
  explicit Context(CPDF_Page* page, bool strict)
      : page(page),
        samples(page->GetDocument(), page->GetResources().Get()),
        strict(strict) {}

  CPDF_Page* const page;
  SampleCache samples;
  PageIndex index;
  std::unique_ptr<CPDF_TextPage> text_page;  // Made on first use.
  // Regions given: forms tagged /INKB_Overlay are an apply's own drawing.
  // Without regions the tag is not trusted.
  const bool strict;
  // Each Type 3 glyph procedure read so far: whether it could be read, and
  // the bounds of what it draws, in glyph space.
  struct Glyph {
    bool readable = false;
    std::optional<CFX_FloatRect> bounds;
  };
  std::map<std::tuple<const CPDF_Font*, uint32_t, const CPDF_Dictionary*>, Glyph>
      glyphs;
  // Where each shading read so far can paint, by its shading object.
  std::map<const CPDF_Object*, OwnBox> shading_boxes;
  bool failed = false;
  // What the sampling tests may still do. Once spent, the inspection fails.
  Work work;
};

void IndexHolder(CPDF_PageObjectHolder* holder,
                 bool overlay,
                 int depth,
                 Context* context) {
  if (depth > kFormLevelLimit) {
    context->failed = true;
    return;
  }
  PageIndex& index = context->index;
  for (auto it = holder->begin(); it != holder->end(); ++it) {
    CPDF_PageObject* object = it->get();
    index.order[object] = index.content_count++;
    if (overlay) {
      index.in_overlay.insert(object);
    }
    if (CPDF_FormObject* form_object = object->AsForm()) {
      if (CPDF_Form* form = form_object->form()) {
        IndexHolder(form, overlay || FormHas(form, kOverlayKey), depth + 1,
                    context);
      }
    }
  }
}

enum class Scope {
  kPage,        // The page's content.
  kAnnotation,  // An annotation's appearance, in its placement.
  kPageCopy,    // A page copy in an annotation, in the page's coordinates.
};

// The part of segment |a|-|b| inside |clip|, or nullopt (Liang-Barsky).
std::optional<std::pair<CFX_PointF, CFX_PointF>> ClipSegment(
    const CFX_PointF& a,
    const CFX_PointF& b,
    const CFX_FloatRect& clip) {
  float t0 = 0;
  float t1 = 1;
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {a.x - clip.left, clip.right - a.x, a.y - clip.bottom,
                      clip.top - a.y};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0) {
      if (q[i] < 0) {
        return std::nullopt;
      }
      continue;
    }
    const float t = q[i] / p[i];
    if (p[i] < 0) {
      t0 = std::max(t0, t);
    } else {
      t1 = std::min(t1, t);
    }
    if (t0 > t1) {
      return std::nullopt;
    }
  }
  return std::make_pair(CFX_PointF(a.x + dx * t0, a.y + dy * t0),
                        CFX_PointF(a.x + dx * t1, a.y + dy * t1));
}

// Points along a path in page space for |visit|: its vertices, and every
// segment sampled at most 1 pt apart where it passes through |clip| (no point
// outside |clip| can matter). Curves are split until each piece is under
// 1 pt across or misses |clip|. Stops when |visit| returns true; returns
// whether it did.
template <typename Visit>
class PathSampler {
 public:
  PathSampler(const CFX_FloatRect& clip, Visit visit, Work* work)
      : clip_(clip), visit_(std::move(visit)), work_(work) {}

  // Whether |p| counts, and true once kMaxSamples points have been tried,
  // when the path counts as present, or once |work_| has run out.
  bool Try(const CFX_PointF& p) {
    if (++samples_ > kMaxSamples || !work_->Spend(1)) {
      return true;
    }
    return visit_(p);
  }

  bool Run(const CPDF_Path& path, const CFX_Matrix& to_page) {
    const std::vector<CFX_Path::Point>& points = path.GetPoints();
    CFX_PointF start;
    CFX_PointF current;
    for (size_t i = 0; i < points.size(); ++i) {
      const CFX_Path::Point& point = points[i];
      const CFX_PointF p = to_page.Transform(point.point_);
      if (point.type_ == CFX_Path::Point::Type::kMove) {
        start = current = p;
        if (Try(p)) {
          return true;
        }
      } else if (point.type_ == CFX_Path::Point::Type::kLine) {
        if (Line(current, p)) {
          return true;
        }
        current = p;
      } else {
        if (i + 2 >= points.size()) {
          return false;
        }
        const CFX_PointF end = to_page.Transform(points[i + 2].point_);
        if (Curve(current, p, to_page.Transform(points[i + 1].point_), end,
                  0)) {
          return true;
        }
        current = end;
        i += 2;
      }
      if (points[i].close_figure_ && Line(current, start)) {
        return true;
      }
    }
    return false;
  }

 private:
  bool Line(const CFX_PointF& a, const CFX_PointF& b) {
    if (Try(b)) {
      return true;
    }
    std::optional<std::pair<CFX_PointF, CFX_PointF>> part =
        ClipSegment(a, b, clip_);
    if (!part.has_value()) {
      return false;
    }
    const CFX_PointF& from = part->first;
    const CFX_PointF& to = part->second;
    const int steps = Steps(std::hypot(to.x - from.x, to.y - from.y));
    if (!steps) {
      return true;
    }
    for (int k = 0; k <= steps; ++k) {
      const float t = static_cast<float>(k) / steps;
      if (Try(CFX_PointF(from.x + (to.x - from.x) * t,
                            from.y + (to.y - from.y) * t))) {
        return true;
      }
    }
    return false;
  }

  bool Curve(const CFX_PointF& p0,
             const CFX_PointF& p1,
             const CFX_PointF& p2,
             const CFX_PointF& p3,
             int depth) {
    CFX_FloatRect hull(p0);
    hull.UpdateRect(p1);
    hull.UpdateRect(p2);
    hull.UpdateRect(p3);
    if (hull.right < clip_.left || hull.left > clip_.right ||
        hull.top < clip_.bottom || hull.bottom > clip_.top) {
      return Try(p3);
    }
    // A piece under 1 pt across is its chord to within that; 2^24 pieces are
    // finer than a float can tell apart.
    if (std::max(hull.Width(), hull.Height()) <= 1 || depth >= 24) {
      return Line(p0, p3);
    }
    const CFX_PointF a((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
    const CFX_PointF b((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
    const CFX_PointF c((p2.x + p3.x) / 2, (p2.y + p3.y) / 2);
    const CFX_PointF ab((a.x + b.x) / 2, (a.y + b.y) / 2);
    const CFX_PointF bc((b.x + c.x) / 2, (b.y + c.y) / 2);
    const CFX_PointF mid((ab.x + bc.x) / 2, (ab.y + bc.y) / 2);
    return Curve(p0, a, ab, mid, depth + 1) || Curve(mid, bc, c, p3, depth + 1);
  }

  const CFX_FloatRect clip_;
  Visit visit_;
  Work* const work_;
  double samples_ = 0;
};

// A clip in effect on what a form holds: the clip of a form object the
// content is painted through, in the space |to_page| places.
struct ClipInEffect {
  CPDF_ClipPath clip;
  CFX_Matrix to_page;
};
using Clips = std::vector<ClipInEffect>;

// The soft mask a form object is painted under, which its content inherits:
// the mask, the matrix it was set under, and what places that matrix on the
// page. A mask lies where it was set, not where the form's matrix would put
// it again.
struct MaskScope {
  const CPDF_Dictionary* mask = nullptr;
  CFX_Matrix matrix;
  CFX_Matrix to_page;
};

class Walker {
 public:
  Walker(Entry* entry, const CPDF_PathRedactor* redactor, Context* context)
      : entry_(entry), redactor_(redactor), context_(context) {}

  // Counts what |holder| holds under the regions. |clips| are the clips of
  // the form objects it is painted through.
  void Holder(CPDF_PageObjectHolder* holder,
              const CFX_Matrix& to_page,
              Scope scope,
              bool counted,
              int depth,
              const Clips& clips,
              const MaskScope* inherited) {
    if (depth > kFormLevelLimit) {
      context_->failed = true;
      return;
    }
    const bool page = scope == Scope::kPage;
    // Drawing counts on the page and in a loupe's copy of it, which holds
    // what the page held when the copy was made. An ordinary appearance is
    // read for text and images only: the apply removes every annotation
    // whose rectangle meets a region, and an appearance cannot paint
    // outside its rectangle.
    const bool draws = page || scope == Scope::kPageCopy;
    for (auto it = holder->begin(); it != holder->end(); ++it) {
      if (context_->work.spent) {
        return;
      }
      CPDF_PageObject* object = it->get();
      if (!object->IsActive() || object == entry_->own_object) {
        continue;
      }
      if (CPDF_FormObject* form_object = object->AsForm()) {
        CPDF_Form* form = form_object->form();
        if (!form || (context_->strict && FormHas(form, kOverlayKey))) {
          continue;
        }
        if (FormHas(form, kPageCopyKey) && !page) {
          // The copy's /Matrix is the page matrix (rotation and box origin),
          // and its content is parsed through it: undo it for page space.
          Holder(form, form->GetDict()->GetMatrixFor("Matrix").GetInverse(),
                 Scope::kPageCopy, true, depth + 1, Clips(), nullptr);
          continue;
        }
        if ((!page || Before(object)) && PaintsIn(object, to_page, clips)) {
          Group(object, to_page, inherited, scope, counted, depth);
        }
        Clips inner = clips;
        if (form_object->clip_path().HasRef()) {
          inner.push_back({form_object->clip_path(), to_page});
        }
        MaskScope mask;
        Holder(form, form_object->form_matrix() * to_page, scope, counted,
               depth + 1, inner, MaskOn(object, to_page, inherited, &mask));
        continue;
      }
      if (page && !Before(object)) {
        continue;
      }
      // What an object carries with it (a mask's group, a pattern's cell, a
      // shading pattern) counts when the object itself paints in a region.
      if (Carries(object) && PaintsIn(object, to_page, clips)) {
        Group(object, to_page, inherited, scope, counted, depth);
        if (counted || draws) {
          std::set<const CPDF_Object*> chain;
          const Found found = CellsOf(object, &chain);
          if (counted) {
            entry_->counts.pattern_content += found.content;
          }
          if (draws && found.drawing) {
            ++entry_->counts.paths;
          }
          if (draws && PictureShadingPattern(object, to_page)) {
            ++entry_->counts.paths;
          }
        }
      }
      if (CPDF_TextObject* text = object->AsText()) {
        if (counted) {
          // A Type 3 glyph may draw outside the box it declares.
          const unsigned chars =
              TextChars(*text, to_page) + Type3Reach(*text, to_page, holder);
          if (page) {
            entry_->counts.text_chars += chars;
            if (in_group_) {
              // Text extraction, the page's second test, does not read a
              // mask's group.
              entry_->counts.text_chars_second += AdvanceChars(*text, to_page);
            }
          } else {
            entry_->counts.annotation_text += chars;
            entry_->counts.annotation_text_second +=
                AdvanceChars(*text, to_page);
          }
        }
      } else if (CPDF_ImageObject* image = object->AsImage()) {
        if (counted && ImageCounts(*image, to_page)) {
          ++(page ? entry_->counts.images_not_blank
                  : entry_->counts.annotation_images);
        }
      } else if (CPDF_PathObject* path = object->AsPath()) {
        if (draws && (PathCrosses(*redactor_, *path, to_page) ||
                     PathSampleInside(*path, to_page))) {
          ++entry_->counts.paths;
        }
      } else if (CPDF_ShadingObject* shading = object->AsShading()) {
        if (draws && (PictureShading(shading->pattern(),
                                    shading->matrix() * to_page) ||
                     ShadingCounts(*shading, to_page))) {
          ++entry_->counts.paths;
        }
      }
    }
  }

  // What INK_Redact_Unremovable counts in |holder|: the objects that paint
  // in a region and carry what the apply cannot rewrite. Forms are read
  // whatever tags they carry: before an apply none is ours to trust.
  void Unremovable(CPDF_PageObjectHolder* holder,
                   const CFX_Matrix& to_page,
                   int depth,
                   const Clips& clips,
                   const MaskScope* inherited) {
    if (depth > kFormLevelLimit) {
      context_->failed = true;
      return;
    }
    for (auto it = holder->begin(); it != holder->end(); ++it) {
      if (context_->work.spent) {
        return;
      }
      CPDF_PageObject* object = it->get();
      if (!object->IsActive()) {
        continue;
      }
      if (CPDF_FormObject* form_object = object->AsForm()) {
        CPDF_Form* form = form_object->form();
        if (!form) {
          continue;
        }
        if (PaintsIn(object, to_page, clips) &&
            GroupHolds(object, to_page, inherited, 0)) {
          ++entry_->mask_groups;
        }
        Clips inner = clips;
        if (form_object->clip_path().HasRef()) {
          inner.push_back({form_object->clip_path(), to_page});
        }
        MaskScope mask;
        Unremovable(form, form_object->form_matrix() * to_page, depth + 1,
                    inner, MaskOn(object, to_page, inherited, &mask));
        continue;
      }
      if (CPDF_TextObject* text = object->AsText()) {
        if (Type3Reach(*text, to_page, holder) &&
            ClipsReach(object, to_page, clips)) {
          ++entry_->glyphs;
        }
      }
      // A carrier counts wherever its object's shape ends, even wholly inside
      // the region: the apply cannot be shown to remove every reference
      // that keeps what the object carries in the file.
      if (!Carries(object) || !PaintsIn(object, to_page, clips)) {
        continue;
      }
      if (PaintsWithTiling(object)) {
        ++entry_->patterns;
      }
      const CPDF_ShadingObject* shading = object->AsShading();
      if ((shading && PictureShading(shading->pattern(),
                                     shading->matrix() * to_page)) ||
          PictureShadingPattern(object, to_page)) {
        ++entry_->shadings;
      }
      // A mask the object inherits is counted once, at the form object it
      // comes through.
      const CPDF_GeneralState& state = object->general_state();
      const bool own_mask =
          !inherited || !state.HasRef() ||
          state.GetSoftMask().Get() != inherited->mask ||
          !state.GetSMaskMatrix() || !(*state.GetSMaskMatrix() == inherited->matrix);
      if (own_mask && GroupHolds(object, to_page, inherited, 0)) {
        ++entry_->mask_groups;
      }
    }
  }

 private:
  // The second test for a path: a vertex or a sampled point of it strictly
  // inside a shrunk region. Paths whose box misses the regions are skipped.
  bool PathSampleInside(const CPDF_PathObject& path,
                        const CFX_Matrix& to_page) {
    const CFX_FloatRect box = PathPaintBox(path, to_page);
    std::optional<CFX_FloatRect> clip;
    for (const RedactRegion& region : entry_->second) {
      if (!Intersects(box, region.bbox)) {
        continue;
      }
      if (clip.has_value()) {
        clip->Union(region.bbox);
      } else {
        clip = region.bbox;
      }
    }
    if (!clip.has_value()) {
      return false;
    }
    auto inside = [this](const CFX_PointF& p) {
      return PointInsideAny(p, entry_->second);
    };
    PathSampler<decltype(inside)> sampler(*clip, inside, &context_->work);
    return sampler.Run(path.path(), path.matrix() * to_page);
  }

  // Whether a shading counts: either test finds it. Both tests read only the
  // shading's box, its clip paths and the matrix, so shadings alike in all
  // three, such as one painted again and again under one clip, are tested
  // once.
  bool ShadingCounts(const CPDF_ShadingObject& shading,
                     const CFX_Matrix& to_page) {
    const CPDF_ClipPath& clip = shading.clip_path();
    const size_t paths = clip.HasRef() ? clip.GetPathCount() : 0;
    if (!context_->work.Spend(1 + paths)) {
      return true;
    }
    const CFX_FloatRect rect = shading.GetRect();
    const float numbers[10] = {rect.left, rect.bottom, rect.right, rect.top,
                               to_page.a, to_page.b,   to_page.c,  to_page.d,
                               to_page.e, to_page.f};
    ShadingKey key;
    // By their bits: a number that is not finite has no order.
    memcpy(key.first.data(), numbers, sizeof(numbers));
    for (size_t i = 0; i < paths; ++i) {
      key.second.emplace_back(clip.GetPath(i).GetObject(),
                              static_cast<int>(clip.GetClipType(i)));
    }
    auto it = shadings_.find(key);
    if (it != shadings_.end()) {
      return it->second.second;
    }
    const bool counts =
        ShadingCrosses(*redactor_, shading, to_page) ||
        ShadingPaintsSample(shading, to_page, entry_->second, &context_->work);
    if (!context_->work.spent) {
      // The clip is kept, so its paths' addresses stay its own.
      shadings_.emplace(std::move(key), std::make_pair(clip, counts));
    }
    return counts;
  }

  bool Before(const CPDF_PageObject* object) const {
    auto it = context_->index.order.find(object);
    return it == context_->index.order.end() || it->second < entry_->before;
  }

  // The group of the soft mask in effect on |object|, and where its content
  // lies on the page: through the matrix in effect when the mask was set. A
  // mask the object inherits from the form object it is painted through lies
  // where that form object's does. False when the object has no such group.
  bool GroupOf(const CPDF_PageObject* object,
               const CFX_Matrix& to_page,
               const MaskScope* inherited,
               RetainPtr<CPDF_Stream>* stream,
               CFX_Matrix* placed) {
    const CPDF_GeneralState& state = object->general_state();
    if (!state.HasRef()) {
      return false;
    }
    RetainPtr<const CPDF_Dictionary> mask = state.GetSoftMask();
    RetainPtr<const CPDF_Stream> group = mask ? mask->GetStreamFor("G") : nullptr;
    if (!group) {
      return false;
    }
    // The form only reads the stream; it takes a mutable one for its type.
    *stream = pdfium::WrapRetain(const_cast<CPDF_Stream*>(group.Get()));
    const CFX_Matrix* set_under = state.GetSMaskMatrix();
    const CFX_Matrix matrix = set_under ? *set_under : CFX_Matrix();
    const bool same = inherited && inherited->mask == mask.Get() &&
                      inherited->matrix == matrix;
    *placed = matrix * (same ? inherited->to_page : to_page);
    return true;
  }

  // The mask scope for the content of form object |object|: its own mask,
  // placed as GroupOf() places it. Null when it has none.
  const MaskScope* MaskOn(const CPDF_PageObject* object,
                          const CFX_Matrix& to_page,
                          const MaskScope* inherited,
                          MaskScope* scope) {
    const CPDF_GeneralState& state = object->general_state();
    RetainPtr<const CPDF_Dictionary> mask =
        state.HasRef() ? state.GetSoftMask() : nullptr;
    if (!mask) {
      return nullptr;
    }
    const CFX_Matrix* set_under = state.GetSMaskMatrix();
    scope->mask = mask.Get();
    scope->matrix = set_under ? *set_under : CFX_Matrix();
    const bool same = inherited && inherited->mask == scope->mask &&
                      inherited->matrix == scope->matrix;
    scope->to_page = same ? inherited->to_page : to_page;
    return scope;
  }

  // A mask's group, parsed, its decoded bytes charged before the parse and
  // its objects after. Null when the stream could not be read.
  std::unique_ptr<CPDF_Form> ParseGroup(RetainPtr<CPDF_Stream> stream) {
    bool fell_back = false;
    if (!ChargeStream(&context_->work, stream.Get(), &fell_back) ||
        context_->work.spent) {
      return nullptr;
    }
    // A group without resources of its own is read with the page's. A name
    // those do not hold is dropped by PDFium, and another reader may find it
    // in a form's: the group is then not read.
    RetainPtr<const CPDF_Dictionary> own =
        stream->GetDict()->GetDictFor("Resources");
    RetainPtr<const CPDF_Dictionary> names =
        own ? own : context_->page->GetResources();
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
    acc->LoadAllDataFiltered();
    const bool resolves = NamesResolve(acc->GetSpan(), names.Get());
    auto form = std::make_unique<CPDF_Form>(
        context_->page->GetDocument(), context_->page->GetMutableResources(),
        std::move(stream));
    form->ParseContent();
    context_->work.Spend(form->GetPageObjectCount());
    if (!resolves || Unreadable(fell_back, *form)) {
      return nullptr;
    }
    return form;
  }

  // Counts what the group of a soft mask on |object| holds under the
  // regions, as content of the holder the object lies in. The caller has
  // found that the object paints in a region. A group in effect on many
  // objects through one matrix is read once. A group whose place is not
  // finite, or that cannot be decoded, counts.
  void Group(const CPDF_PageObject* object,
             const CFX_Matrix& to_page,
             const MaskScope* inherited,
             Scope scope,
             bool counted,
             int depth) {
    RetainPtr<CPDF_Stream> stream;
    CFX_Matrix placed;
    if (!GroupOf(object, to_page, inherited, &stream, &placed)) {
      return;
    }
    const float numbers[6] = {placed.a, placed.b, placed.c,
                              placed.d, placed.e, placed.f};
    GroupKey key;
    std::get<0>(key) = stream.Get();
    memcpy(std::get<1>(key).data(), numbers, sizeof(numbers));
    std::get<2>(key) = static_cast<int>(scope);
    std::get<3>(key) = counted;
    if (!groups_.insert(key).second || group_stack_.count(stream.Get())) {
      return;
    }
    const CPDF_Stream* id = stream.Get();
    std::unique_ptr<CPDF_Form> form =
        Finite(placed) ? ParseGroup(std::move(stream)) : nullptr;
    if (context_->work.spent) {
      return;
    }
    if (!form) {
      ++(scope == Scope::kPage ? entry_->counts.paths
                               : entry_->counts.annotation_text);
      return;
    }
    group_stack_.insert(id);
    ++in_group_;
    Holder(form.get(), placed, scope, counted, depth + 1, Clips(), nullptr);
    --in_group_;
    group_stack_.erase(id);
  }

  // Whether |path| is one rectangle, at any angle: a clip a fade's writer
  // adds without meaning a shape, such as the group's own box under a
  // rotated matrix.
  static bool Rectangle(const CPDF_Path& path) {
    if (!path.HasRef()) {
      return false;
    }
    if (path.GetRectOppositeCorner().has_value()) {
      return true;
    }
    const std::vector<CFX_Path::Point>& points = path.GetPoints();
    std::vector<CFX_PointF> corners;
    for (size_t i = 0; i < points.size(); ++i) {
      if (points[i].type_ == CFX_Path::Point::Type::kBezier ||
          (i > 0 && points[i].type_ == CFX_Path::Point::Type::kMove)) {
        return false;
      }
      if (corners.empty() || !(points[i].point_ == corners.back())) {
        corners.push_back(points[i].point_);
      }
    }
    if (corners.size() == 5 && corners[4] == corners[0]) {
      corners.pop_back();
    }
    if (corners.size() != 4) {
      return false;
    }
    const CFX_PointF side = corners[1] - corners[0];
    const CFX_PointF next = corners[2] - corners[1];
    const CFX_PointF closing = corners[0] + corners[2] - corners[1] - corners[3];
    const float scale = side.x * side.x + side.y * side.y + next.x * next.x +
                        next.y * next.y;
    return scale > 0 &&
           std::fabs(side.x * next.x + side.y * next.y) <= 0.001f * scale &&
           closing.x * closing.x + closing.y * closing.y <= 0.000001f * scale;
  }

  // Whether a group draws nothing but a fade, which is how gradient
  // transparency is drawn: at most four objects, each a filled axis-aligned
  // rectangle or a plain ramp, under no clip but rectangles.
  // Text, an image, any other path, a fifth object, a shading that can hold
  // a picture, a pattern, a form, a mask of its own or a clip to a shape
  // makes it more than that: a ramp through a shape is the shape, and many
  // small rectangles or clipped ramps can draw anything.
  bool FadeOnly(const CPDF_Form& form) {
    int objects = 0;
    for (auto it = form.begin(); it != form.end(); ++it) {
      const CPDF_PageObject* object = it->get();
      if (!object->IsActive()) {
        continue;
      }
      if (++objects > 4) {
        return false;
      }
      const CPDF_GeneralState& state = object->general_state();
      if (state.HasRef() && state.GetSoftMask()) {
        return false;
      }
      const CPDF_ClipPath& clip = object->clip_path();
      if (clip.HasRef()) {
        if (clip.GetTextCount() > 0) {
          return false;
        }
        for (size_t i = 0; i < clip.GetPathCount(); ++i) {
          if (!Rectangle(clip.GetPath(i))) {
            return false;
          }
        }
      }
      if (const CPDF_ShadingObject* shading = object->AsShading()) {
        if (!Ramp(shading->pattern())) {
          return false;
        }
        continue;
      }
      const CPDF_PathObject* path = object->AsPath();
      if (!path || path->has_no_filltype() || path->stroke() ||
          !path->path().GetRectOppositeCorner().has_value()) {
        return false;
      }
      const CFX_Matrix& m = path->matrix();
      const CPDF_Color* fill = path->color_state().GetFillColor();
      if (!((m.b == 0 && m.c == 0) || (m.a == 0 && m.d == 0)) || !fill ||
          fill->IsPattern()) {
        return false;
      }
    }
    return true;
  }

  // Whether the group of a soft mask on |object| holds anything in a
  // region: an object of the group, or of a group masking one of those,
  // whose box meets one when placed by the mask's matrix. A group that only
  // draws a fade holds nothing. True when the place cannot be worked out or
  // the group cannot be decoded. False without a group.
  bool GroupHolds(const CPDF_PageObject* object,
                  const CFX_Matrix& to_page,
                  const MaskScope* inherited,
                  int depth) {
    RetainPtr<CPDF_Stream> stream;
    CFX_Matrix placed;
    if (!GroupOf(object, to_page, inherited, &stream, &placed)) {
      return false;
    }
    if (!Finite(placed) || depth > kFormLevelLimit) {
      return true;
    }
    if (group_stack_.count(stream.Get())) {
      return false;
    }
    const float numbers[6] = {placed.a, placed.b, placed.c,
                              placed.d, placed.e, placed.f};
    std::pair<const CPDF_Stream*, std::array<uint32_t, 6>> key;
    key.first = stream.Get();
    memcpy(key.second.data(), numbers, sizeof(numbers));
    auto it = group_holds_.find(key);
    if (it != group_holds_.end()) {
      return it->second;
    }
    const CPDF_Stream* id = stream.Get();
    std::unique_ptr<CPDF_Form> form = ParseGroup(std::move(stream));
    if (context_->work.spent) {
      return true;
    }
    bool holds = !form;
    if (form && !FadeOnly(*form)) {
      group_stack_.insert(id);
      for (auto item = form->begin(); item != form->end() && !holds; ++item) {
        const CPDF_PageObject* inner = item->get();
        if (!inner->IsActive()) {
          continue;
        }
        const CFX_FloatRect box = ShapeBox(inner, placed);
        holds = !Finite(box) || RectMeetsAnyBBox(box, entry_->inset) ||
                GroupHolds(inner, placed, nullptr, depth + 1);
      }
      group_stack_.erase(id);
    }
    if (!context_->work.spent) {
      group_holds_.emplace(key, holds);
    }
    return holds;
  }

  // Whether a shading that can hold a picture (anything but a plain ramp:
  // type 1, types 4 to 7, an axial or radial shading whose function is not
  // plain, and any other type, to fail closed) lies under the regions: its own
  // box, placed by |to_page| from the shading's space, meets one. Clips do
  // not count, since the apply's own clip would hide what it left in the
  // file. True when the shading's geometry cannot be read.
  bool PictureShading(const CPDF_ShadingPattern* pattern,
                      const CFX_Matrix& to_page) {
    if (!pattern) {
      return true;
    }
    if (Ramp(pattern)) {
      return false;
    }
    const OwnBox own = OwnBoxOf(*pattern);
    if (!own.known) {
      return true;
    }
    if (!(own.box.Width() > 0) || !(own.box.Height() > 0)) {
      return false;
    }
    CFX_FloatRect box = to_page.TransformRect(own.box);
    box.Normalize();
    return !Finite(box) || RectMeetsAnyBBox(box, entry_->inset);
  }

  // Where |pattern| can paint, clips aside, read once in a call. Not known
  // when it cannot be read or the work ran out.
  OwnBox OwnBoxOf(const CPDF_ShadingPattern& pattern) {
    RetainPtr<const CPDF_Object> key = pattern.GetShadingObject();
    auto it = context_->shading_boxes.find(key.Get());
    if (it == context_->shading_boxes.end()) {
      const OwnBox own = ShadingOwnBox(pattern, &context_->work);
      if (context_->work.spent) {
        return {};
      }
      it = context_->shading_boxes.emplace(key.Get(), own).first;
    }
    return it->second;
  }

  // Whether |object| paints with a shading pattern that can hold a picture
  // under the regions. The caller has found that the object paints in one.
  bool PictureShadingPattern(CPDF_PageObject* object,
                             const CFX_Matrix& to_page) {
    const CPDF_ColorState& colors = object->color_state();
    if (!colors.HasRef()) {
      return false;
    }
    for (bool stroke : {false, true}) {
      const CPDF_Color* color =
          stroke ? colors.GetStrokeColor() : colors.GetFillColor();
      if (!color || !color->IsPattern() || !Paints(object, stroke)) {
        continue;
      }
      RetainPtr<CPDF_Pattern> pattern = color->GetPattern();
      CPDF_ShadingPattern* shading =
          pattern ? pattern->AsShadingPattern() : nullptr;
      if (!shading) {
        continue;
      }
      if (!shading->Load() ||
          PictureShading(shading, shading->pattern_to_form() * to_page)) {
        return true;
      }
    }
    return false;
  }

  // What a pattern's cell holds.
  struct Found {
    void Add(const Found& other) {
      content += other.content;
      drawing = drawing || other.drawing;
    }

    unsigned content = 0;  // Characters and images.
    bool drawing = false;  // A path, a shading or a pattern of its own.
  };

  // Whether |object| paints with its fill colour, or with its stroke colour:
  // a path as it is filled and stroked, text as its rendering mode says, an
  // image only when it is a stencil mask, which the fill colour paints.
  static bool Paints(const CPDF_PageObject* object, bool stroke) {
    if (const CPDF_PathObject* path = object->AsPath()) {
      return stroke ? path->stroke() : !path->has_no_filltype();
    }
    if (const CPDF_TextObject* text = object->AsText()) {
      switch (text->GetTextRenderMode()) {
        case TextRenderingMode::MODE_FILL:
        case TextRenderingMode::MODE_FILL_CLIP:
          return !stroke;
        case TextRenderingMode::MODE_STROKE:
        case TextRenderingMode::MODE_STROKE_CLIP:
          return stroke;
        case TextRenderingMode::MODE_FILL_STROKE:
        case TextRenderingMode::MODE_FILL_STROKE_CLIP:
          return true;
        case TextRenderingMode::MODE_INVISIBLE:
        case TextRenderingMode::MODE_CLIP:
          return false;
        case TextRenderingMode::MODE_UNKNOWN:
          return true;
      }
    }
    if (const CPDF_ImageObject* image = object->AsImage()) {
      RetainPtr<CPDF_Image> stencil = image->GetImage();
      return !stroke && stencil && stencil->IsMask();
    }
    return false;
  }

  // A box, stretching past any region.
  static CFX_FloatRect Everywhere() {
    return CFX_FloatRect(-1e30f, -1e30f, 1e30f, 1e30f);
  }

  // The box of |object|'s own shape on the page, clips aside, its stroke
  // included. A path's is built from its outline and stroke. Text grows by
  // half its line width under the largest stretch in effect when its mode
  // strokes; a hairline is a point wide on the page. A shading painted with
  // sh has the rectangle its holder gave it: its clip's box, or the page's
  // or the form's. In a form or group without /BBox, and under no clip,
  // that rectangle is empty though PDFium paints the shading: it is then
  // bounded by the shading's own box, or by nothing when that is not known.
  CFX_FloatRect ShapeBox(const CPDF_PageObject* object,
                         const CFX_Matrix& to_page) {
    if (const CPDF_PathObject* path = object->AsPath()) {
      return PathPaintBox(*path, to_page);
    }
    if (const CPDF_ShadingObject* shading = object->AsShading()) {
      const CFX_FloatRect rect = shading->GetRect();
      const CPDF_ClipPath& clip = shading->clip_path();
      if ((rect.Width() > 0 && rect.Height() > 0) ||
          (clip.HasRef() && clip.GetPathCount() > 0)) {
        CFX_FloatRect box = to_page.TransformRect(rect);
        box.Normalize();
        return box;
      }
      const OwnBox own =
          shading->pattern() ? OwnBoxOf(*shading->pattern()) : OwnBox();
      if (!own.known) {
        return Everywhere();
      }
      CFX_FloatRect box = (shading->matrix() * to_page).TransformRect(own.box);
      box.Normalize();
      return box;
    }
    CFX_FloatRect box = to_page.TransformRect(object->GetRect());
    box.Normalize();
    if (const CPDF_TextObject* text = object->AsText()) {
      if (Paints(object, true)) {
        const float width = std::fabs(object->graph_state().GetLineWidth());
        const float grow =
            width > 0 ? width / 2 *
                            std::max(Stretch(to_page),
                                     Stretch(text->GetTextMatrix() * to_page))
                      : 0.5f;
        box.Inflate(grow, grow);
      }
    }
    return box;
  }

  // Whether |object| paints with a pattern colour: a tiling pattern, or
  // with |shading|, a shading pattern.
  static bool PaintsWithPattern(const CPDF_PageObject* object, bool shading) {
    const CPDF_ColorState& colors = object->color_state();
    if (!colors.HasRef()) {
      return false;
    }
    for (bool stroke : {false, true}) {
      const CPDF_Color* color =
          stroke ? colors.GetStrokeColor() : colors.GetFillColor();
      if (!color || !color->IsPattern() || !Paints(object, stroke)) {
        continue;
      }
      RetainPtr<CPDF_Pattern> pattern = color->GetPattern();
      if (pattern && (shading ? !!pattern->AsShadingPattern()
                              : !!pattern->AsTilingPattern())) {
        return true;
      }
    }
    return false;
  }

  static bool PaintsWithTiling(const CPDF_PageObject* object) {
    return PaintsWithPattern(object, false);
  }

  // Whether |object| brings with it something the redaction does not enter:
  // a pattern, a shading that can hold a picture, or a soft mask's group.
  static bool Carries(const CPDF_PageObject* object) {
    if (PaintsWithPattern(object, false) || PaintsWithPattern(object, true)) {
      return true;
    }
    if (const CPDF_ShadingObject* shading = object->AsShading()) {
      if (!Ramp(shading->pattern())) {
        return true;
      }
    }
    const CPDF_GeneralState& state = object->general_state();
    return state.HasRef() && state.GetSoftMask();
  }

  // Whether |clip|, placed by |to_page|, lets anything be painted in a
  // region: each of its paths crosses one, by the redactor's test. Text
  // that clips is not read; it is taken to let everything through.
  bool ClipReaches(const CPDF_ClipPath& clip, const CFX_Matrix& to_page) {
    if (!clip.HasRef()) {
      return true;
    }
    const float numbers[6] = {to_page.a, to_page.b, to_page.c,
                              to_page.d, to_page.e, to_page.f};
    for (size_t i = 0; i < clip.GetPathCount(); ++i) {
      const CPDF_Path path = clip.GetPath(i);
      ClipKey key;
      std::get<0>(key) = path.GetObject();
      std::get<1>(key) = static_cast<int>(clip.GetClipType(i));
      memcpy(std::get<2>(key).data(), numbers, sizeof(numbers));
      auto it = clip_reach_.find(key);
      if (it == clip_reach_.end()) {
        bool reaches = false;
        if (path.HasRef() && !path.GetPoints().empty()) {
          CFX_FloatRect box = to_page.TransformRect(path.GetBoundingBox());
          box.Normalize();
          if (!Finite(box)) {
            reaches = true;
          } else if (RectMeetsAnyBBox(box, entry_->inset)) {
            const PathRedactionResult result = redactor_->Redact(
                Probe(path, clip.GetClipType(i)).get(), to_page);
            reaches = !result.succeeded || result.changed;
          }
        }
        // The path is kept, so its address stays its own.
        it = clip_reach_.emplace(key, std::make_pair(path, reaches)).first;
      }
      if (!it->second.second) {
        return false;
      }
    }
    return true;
  }

  // Whether the clips on |object|, its own and those of the form objects it
  // is painted through, let it paint in a region.
  bool ClipsReach(const CPDF_PageObject* object,
                  const CFX_Matrix& to_page,
                  const Clips& clips) {
    if (!ClipReaches(object->clip_path(), to_page)) {
      return false;
    }
    for (const ClipInEffect& clip : clips) {
      if (!ClipReaches(clip.clip, clip.to_page)) {
        return false;
      }
    }
    return true;
  }

  // Whether |object| paints in a region: its box meets one, every clip in
  // effect reaches one, and so does its own shape (a path by the redactor's
  // test and the sampler, text by its glyphs' boxes, an image by its
  // quad). Each part is tested against the region on its own, so an object
  // and a clip that both reach a region in different places count: the
  // answer errs towards yes. An object whose place is not finite paints.
  bool PaintsIn(CPDF_PageObject* object,
                const CFX_Matrix& to_page,
                const Clips& clips) {
    const CFX_FloatRect box = ShapeBox(object, to_page);
    if (!Finite(box)) {
      return true;
    }
    if (!RectMeetsAnyBBox(box, entry_->inset) ||
        !ClipsReach(object, to_page, clips)) {
      return false;
    }
    if (CPDF_PathObject* path = object->AsPath()) {
      return PathCrosses(*redactor_, *path, to_page) ||
             PathSampleInside(*path, to_page);
    }
    if (CPDF_TextObject* text = object->AsText()) {
      // Stroked text paints past its glyphs' boxes by its stroke: its box,
      // grown by that, decides.
      return Paints(object, true) || TextChars(*text, to_page) > 0 ||
             AdvanceChars(*text, to_page) > 0;
    }
    if (CPDF_ImageObject* image = object->AsImage()) {
      return QuadMeetsAny(Transformed(CFX_FloatRect(0, 0, 1, 1),
                                      image->matrix() * to_page),
                          entry_->inset);
    }
    return true;
  }

  // What the cells of the tiling patterns |object| paints with hold,
  // wherever in the cell: characters and images, and whether they draw.
  // The redaction does not enter a pattern's stream, so the caller counts
  // this for an object that paints in a region. Patterns used inside a cell
  // are followed; |chain| holds the patterns being read, so a pattern that
  // paints with itself ends. A pattern's cell is read once in a call,
  // wherever it is used, and nothing is parsed once the work has run out.
  Found CellsOf(CPDF_PageObject* object, std::set<const CPDF_Object*>* chain) {
    Found found;
    const CPDF_ColorState& colors = object->color_state();
    if (!colors.HasRef()) {
      return found;
    }
    for (bool stroke : {false, true}) {
      const CPDF_Color* color =
          stroke ? colors.GetStrokeColor() : colors.GetFillColor();
      if (!color || !color->IsPattern() || !Paints(object, stroke)) {
        continue;
      }
      RetainPtr<CPDF_Pattern> pattern = color->GetPattern();
      CPDF_TilingPattern* tiling = pattern ? pattern->AsTilingPattern() : nullptr;
      if (!tiling || chain->count(tiling->pattern_obj().Get())) {
        continue;
      }
      if (chain->size() >= kMaxPatternChain) {
        context_->failed = true;
        return found;
      }
      auto it = cells_.find(tiling->pattern_obj().Get());
      if (it != cells_.end()) {
        found.Add(it->second.first);
        continue;
      }
      // A cell that could not be read counts as holding both.
      Found cell_found;
      cell_found.content = 1;
      cell_found.drawing = true;
      bool fell_back = false;
      if (ChargeStream(&context_->work, ToStream(tiling->pattern_obj().Get()),
                       &fell_back) &&
          !context_->work.spent) {
        chain->insert(tiling->pattern_obj().Get());
        // A pattern's content is parsed on its own, from level 0.
        std::unique_ptr<CPDF_Form> cell = tiling->Load(object);
        if (cell && !Unreadable(fell_back, *cell)) {
          context_->work.Spend(cell->GetPageObjectCount());
          cell_found.content = PatternContent(cell.get(), chain, 0);
          cell_found.drawing = HasDrawing(cell.get(), 0);
        }
        chain->erase(tiling->pattern_obj().Get());
      }
      if (context_->work.spent) {
        return found;
      }
      {
        // The pattern is kept, so its object's address stays its own.
        cells_.emplace(tiling->pattern_obj().Get(),
                       std::make_pair(cell_found, pdfium::WrapRetain(tiling)));
      }
      found.Add(cell_found);
    }
    return found;
  }

  // Whether a cell holds a path, a shading, or an object painted with a
  // pattern of its own.
  bool HasDrawing(CPDF_PageObjectHolder* holder, int depth) {
    if (depth > kFormLevelLimit) {
      context_->failed = true;
      return true;
    }
    for (auto it = holder->begin(); it != holder->end(); ++it) {
      CPDF_PageObject* object = it->get();
      if (!context_->work.Spend(1)) {
        return true;
      }
      if (object->AsPath() || object->AsShading()) {
        return true;
      }
      const CPDF_ColorState& colors = object->color_state();
      if (colors.HasRef()) {
        for (const CPDF_Color* color :
             {colors.GetFillColor(), colors.GetStrokeColor()}) {
          if (color && color->IsPattern()) {
            return true;
          }
        }
      }
      if (CPDF_FormObject* form = object->AsForm()) {
        if (form->form() && HasDrawing(form->form(), depth + 1)) {
          return true;
        }
      }
    }
    return false;
  }

  // What the procedure of glyph |code| of |font| draws, read from the font's
  // own dictionary the first time in a call. PDFium's cached glyph is not
  // asked: rendering a page drops the parsed form of a glyph that is one
  // image, and the answer must not turn on whether the page was drawn. A
  // font with resources of its own is read with those; one without, with
  // |shown_with|, the resources of the content that shows the glyph. A
  // procedure naming a resource those do not hold is not readable.
  const Context::Glyph& GlyphOf(CPDF_Type3Font* font,
                                uint32_t code,
                                RetainPtr<CPDF_Dictionary> shown_with) {
    RetainPtr<const CPDF_Dictionary> dict = font->GetFontDict();
    RetainPtr<const CPDF_Dictionary> own =
        dict ? dict->GetDictFor("Resources") : nullptr;
    RetainPtr<CPDF_Dictionary> resources =
        own && !CPDF_Type3Font::GlyphsUseContentResources(dict.Get())
            ? pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(own.Get()))
            : std::move(shown_with);
    const auto key =
        std::make_tuple(static_cast<const CPDF_Font*>(font), code,
                        static_cast<const CPDF_Dictionary*>(resources.Get()));
    auto it = context_->glyphs.find(key);
    if (it != context_->glyphs.end()) {
      return it->second;
    }
    Context::Glyph glyph;
    RetainPtr<const CPDF_Dictionary> procs =
        dict ? dict->GetDictFor("CharProcs") : nullptr;
    const char* name = font->GetCharProcName(code);
    RetainPtr<const CPDF_Object> entry =
        name && procs ? procs->GetDirectObjectFor(name) : nullptr;
    if (!entry) {
      // No procedure: the glyph draws nothing.
      glyph.readable = true;
    } else if (const CPDF_Stream* proc = entry->AsStream()) {
      bool fell_back = false;
      if (ChargeStream(&context_->work, proc, &fell_back) &&
          !context_->work.spent) {
        auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(proc));
        acc->LoadAllDataFiltered();
        const bool resolves = NamesResolve(acc->GetSpan(), resources.Get());
        // The form only reads the stream; it takes a mutable one for its type.
        CPDF_Form form(context_->page->GetDocument(), std::move(resources),
                       pdfium::WrapRetain(const_cast<CPDF_Stream*>(proc)));
        CPDF_Type3Char scratch;
        form.ParseContentForType3Char(&scratch);
        context_->work.Spend(form.GetPageObjectCount());
        glyph.readable = resolves && !Unreadable(fell_back, form);
        for (auto item = form.begin(); item != form.end(); ++item) {
          const CPDF_PageObject* object = item->get();
          if (!object->IsActive()) {
            continue;
          }
          // A path by its outline and stroke. A shading by its own box:
          // a glyph is drawn under no clip but the text's, and the font's
          // box, which the file supplies, bounds nothing. A shading with no
          // box of its own, and a form without one, reach anywhere.
          CFX_FloatRect rect = object->GetRect();
          if (const CPDF_PathObject* path = object->AsPath()) {
            rect = PathPaintBox(*path, CFX_Matrix());
          } else if (const CPDF_ShadingObject* shading = object->AsShading()) {
            const OwnBox box =
                shading->pattern() ? OwnBoxOf(*shading->pattern()) : OwnBox();
            if (!box.known) {
              glyph.readable = false;
            }
            rect = shading->matrix().TransformRect(box.box);
            rect.Normalize();
          } else if (object->AsForm() &&
                     !(rect.Width() > 0 && rect.Height() > 0)) {
            glyph.readable = false;
          }
          if (glyph.bounds.has_value()) {
            glyph.bounds->Union(rect);
          } else {
            glyph.bounds = rect;
          }
        }
      }
    }
    return context_->glyphs.emplace(key, glyph).first->second;
  }

  // The glyphs of Type 3 |text|, shown by content of |holder|, whose
  // procedure paints in a region though the box the glyph declares does not
  // meet it. The text redactor goes by the declared box: a glyph whose box
  // meets a region is removed with its text, whatever its procedure holds,
  // and is not counted here. A procedure that cannot be read is taken to
  // paint in the region.
  unsigned Type3Reach(CPDF_TextObject& text,
                      const CFX_Matrix& to_page,
                      CPDF_PageObjectHolder* holder) {
    RetainPtr<CPDF_Font> font = text.GetFont();
    CPDF_Type3Font* type3 = font ? font->AsType3Font() : nullptr;
    // Text in a mode that neither fills nor strokes paints nothing.
    if (!type3 || (!Paints(&text, false) && !Paints(&text, true))) {
      return 0;
    }
    const float size = text.GetFontSize();
    const CFX_Matrix matrix = text.GetTextMatrix() * to_page;
    unsigned count = 0;
    for (size_t i = 0; i < text.CharCount(); ++i) {
      const CPDF_TextObject::Item item = text.GetItemInfo(i);
      if (item.char_code_ == CPDF_Font::kInvalidCharCode ||
          !context_->work.Spend(1)) {
        continue;
      }
      const Context::Glyph& glyph =
          GlyphOf(type3, item.char_code_, holder->GetMutableResources());
      if (glyph.readable && !glyph.bounds.has_value()) {
        continue;
      }
      const auto meets = [&](const CFX_FloatRect& local) {
        const Quad quad = Transformed(local, matrix);
        for (const CFX_PointF& point : quad) {
          if (!Finite(point)) {
            return true;
          }
        }
        return QuadMeetsAny(quad, entry_->regions);
      };
      const FX_RECT units = font->GetCharBBox(item.char_code_);
      const bool declared = meets(CFX_FloatRect(
          units.left * size / 1000.0f + item.origin_.x,
          units.bottom * size / 1000.0f + item.origin_.y,
          units.right * size / 1000.0f + item.origin_.x,
          units.top * size / 1000.0f + item.origin_.y));
      bool reaches = true;
      if (glyph.readable) {
        CFX_FloatRect drawn =
            type3->GetFontMatrix().TransformRect(*glyph.bounds);
        drawn.Normalize();
        reaches = meets(CFX_FloatRect(drawn.left * size + item.origin_.x,
                                      drawn.bottom * size + item.origin_.y,
                                      drawn.right * size + item.origin_.x,
                                      drawn.top * size + item.origin_.y));
      }
      if (reaches && !declared) {
        ++count;
      }
    }
    return count;
  }

  // The characters and images a cell holds, patterns its objects paint with
  // followed.
  unsigned PatternContent(CPDF_PageObjectHolder* holder,
                          std::set<const CPDF_Object*>* chain,
                          int depth) {
    if (depth > kFormLevelLimit) {
      context_->failed = true;
      return 0;
    }
    unsigned count = 0;
    for (auto it = holder->begin(); it != holder->end(); ++it) {
      if (context_->work.spent) {
        return count;
      }
      CPDF_PageObject* object = it->get();
      count += CellsOf(object, chain).content;
      if (CPDF_TextObject* text = object->AsText()) {
        count += static_cast<unsigned>(text->CharCount());
      } else if (object->AsImage()) {
        ++count;
      } else if (CPDF_FormObject* form = object->AsForm()) {
        if (form->form()) {
          count += PatternContent(form->form(), chain, depth + 1);
        }
      }
    }
    return count;
  }

  // The redactor's test: each glyph's box from the font's glyph bounds.
  unsigned TextChars(CPDF_TextObject& text, const CFX_Matrix& to_page) {
    RetainPtr<CPDF_Font> font = text.GetFont();
    if (!font) {
      return 0;
    }
    const float size = text.GetFontSize();
    const CFX_Matrix matrix = text.GetTextMatrix() * to_page;
    unsigned count = 0;
    for (size_t i = 0; i < text.CharCount(); ++i) {
      const CPDF_TextObject::Item item = text.GetItemInfo(i);
      const FX_RECT units = font->GetCharBBox(item.char_code_);
      const CFX_FloatRect local(
          units.left * size / 1000.0f + item.origin_.x,
          units.bottom * size / 1000.0f + item.origin_.y,
          units.right * size / 1000.0f + item.origin_.x,
          units.top * size / 1000.0f + item.origin_.y);
      if (QuadMeetsAny(Transformed(local, matrix), entry_->regions)) {
        ++count;
      }
    }
    return count;
  }

  // The second test for text outside the page content: each glyph's box
  // from its advance and the font's ascent and descent, against the shrunk
  // regions.
  unsigned AdvanceChars(CPDF_TextObject& text, const CFX_Matrix& to_page) {
    RetainPtr<CPDF_Font> font = text.GetFont();
    if (!font) {
      return 0;
    }
    const float size = text.GetFontSize();
    float ascent = font->GetTypeAscent() * size / 1000.0f;
    float descent = font->GetTypeDescent() * size / 1000.0f;
    if (!(ascent > descent)) {
      ascent = size;
      descent = 0;
    }
    const CFX_Matrix matrix = text.GetTextMatrix() * to_page;
    unsigned count = 0;
    for (size_t i = 0; i < text.CharCount(); ++i) {
      const CPDF_TextObject::Item item = text.GetItemInfo(i);
      if (item.char_code_ == CPDF_Font::kInvalidCharCode) {
        continue;
      }
      float width = text.GetCharWidth(item.char_code_);
      if (!(width > 0)) {
        width = size / 2;
      }
      const CFX_FloatRect local(item.origin_.x, item.origin_.y + descent,
                                item.origin_.x + width,
                                item.origin_.y + ascent);
      if (QuadMeetsAny(Transformed(local, matrix), entry_->second)) {
        ++count;
      }
    }
    return count;
  }

  static Quad Transformed(const CFX_FloatRect& r, const CFX_Matrix& m) {
    return {m.Transform(CFX_PointF(r.left, r.top)),
            m.Transform(CFX_PointF(r.right, r.top)),
            m.Transform(CFX_PointF(r.left, r.bottom)),
            m.Transform(CFX_PointF(r.right, r.bottom))};
  }

  bool ImageCounts(const CPDF_ImageObject& image, const CFX_Matrix& to_page) {
    const CFX_Matrix image_to_page = image.matrix() * to_page;
    CFX_FloatRect box = image_to_page.TransformRect(CFX_FloatRect(0, 0, 1, 1));
    box.Normalize();
    if (!RectMeetsAnyBBox(box, entry_->regions)) {
      return false;
    }
    return !ImageBlank(&context_->samples, &image, to_page, entry_->regions,
                       context_->strict);
  }

  // A shading's box and matrix, and its clip paths with their fill rules.
  using ShadingKey =
      std::pair<std::array<uint32_t, 10>,
                std::vector<std::pair<const CFX_Path*, int>>>;

  Entry* const entry_;
  const CPDF_PathRedactor* const redactor_;
  Context* const context_;
  std::map<ShadingKey, std::pair<CPDF_ClipPath, bool>> shadings_;
  // What each pattern's cell holds.
  std::map<const CPDF_Object*, std::pair<Found, RetainPtr<CPDF_TilingPattern>>>
      cells_;
  // A clip path, its fill rule and its place, and whether it reaches a region.
  using ClipKey = std::tuple<const CFX_Path*, int, std::array<uint32_t, 6>>;
  std::map<ClipKey, std::pair<CPDF_Path, bool>> clip_reach_;
  std::map<std::pair<const CPDF_Stream*, std::array<uint32_t, 6>>, bool>
      group_holds_;
  // A mask's group, where it lies, the scope and whether it is counted.
  using GroupKey =
      std::tuple<const CPDF_Stream*, std::array<uint32_t, 6>, int, bool>;
  std::set<GroupKey> groups_;
  // The groups being read, so that one that masks its own content ends.
  std::set<const CPDF_Stream*> group_stack_;
  int in_group_ = 0;
};

// The second test for page text: the boxes PDFium's text extraction gives
// each character (its tight and loose boxes together), against the shrunk
// regions. Text extraction reads the page content and its forms.
unsigned ExtractedChars(Context* context, const Entry& entry) {
  if (!context->text_page) {
    context->text_page = std::make_unique<CPDF_TextPage>(context->page, false);
  }
  const CPDF_TextPage& text_page = *context->text_page;
  const PageIndex& index = context->index;
  unsigned count = 0;
  for (int i = 0; i < text_page.CountChars(); ++i) {
    const CPDF_TextPage::CharInfo& info = text_page.GetCharInfo(i);
    if (info.char_type() == CPDF_TextPage::CharType::kGenerated) {
      continue;
    }
    if (const CPDF_TextObject* object = info.text_object()) {
      if (context->strict && index.in_overlay.count(object)) {
        continue;
      }
      auto it = index.order.find(object);
      if (it != index.order.end() && it->second >= entry.before) {
        continue;
      }
    }
    CFX_FloatRect box = info.char_box();
    box.Normalize();
    CFX_FloatRect loose = info.loose_char_box();
    loose.Normalize();
    if (box.IsEmpty()) {
      box = loose;
    } else if (!loose.IsEmpty()) {
      box.Union(loose);
    }
    if (!box.IsEmpty() && QuadMeetsAny(QuadFromRect(box), entry.second)) {
      ++count;
    }
  }
  return count;
}

// The streams of an annotation's appearances: every state of /N, /D and /R.
std::vector<RetainPtr<CPDF_Stream>> AppearanceStreams(
    const CPDF_Dictionary* annot) {
  std::vector<RetainPtr<CPDF_Stream>> streams;
  RetainPtr<const CPDF_Dictionary> ap = annot->GetDictFor("AP");
  if (!ap) {
    return streams;
  }
  for (const char* mode : {"N", "D", "R"}) {
    RetainPtr<const CPDF_Object> entry = ap->GetDirectObjectFor(mode);
    if (!entry) {
      continue;
    }
    if (const CPDF_Stream* stream = entry->AsStream()) {
      streams.push_back(pdfium::WrapRetain(const_cast<CPDF_Stream*>(stream)));
    } else if (const CPDF_Dictionary* states = entry->AsDictionary()) {
      CPDF_DictionaryLocker locker(states);
      for (const auto& [name, value] : locker) {
        RetainPtr<const CPDF_Stream> stream = ToStream(value->GetDirect());
        if (stream) {
          streams.push_back(
              pdfium::WrapRetain(const_cast<CPDF_Stream*>(stream.Get())));
        }
      }
    }
  }
  return streams;
}

// An appearance stream, parsed, and the matrix that places it on the page,
// as CPDF_Annot draws it.
std::unique_ptr<CPDF_Form> Appearance(CPDF_Page* page,
                                      const CPDF_Dictionary* annot,
                                      RetainPtr<CPDF_Stream> stream,
                                      CFX_Matrix* to_page) {
  // The form only reads the stream; it takes a mutable one for its type.
  auto form = std::make_unique<CPDF_Form>(
      page->GetDocument(), page->GetMutableResources(), std::move(stream));
  form->ParseContent();
  RetainPtr<const CPDF_Dictionary> dict = form->GetDict();
  CFX_FloatRect bbox = dict->GetMatrixFor("Matrix").TransformRect(
      dict->GetRectFor("BBox"));
  CFX_FloatRect rect = annot->GetRectFor("Rect");
  rect.Normalize();
  to_page->MatchRect(rect, bbox);
  return form;
}

std::vector<RedactRegion> RegionsOfRedact(const CPDF_Dictionary* annot) {
  std::vector<RedactRegion> regions;
  RetainPtr<const CPDF_Array> points = annot->GetArrayFor("QuadPoints");
  if (points && points->size() >= 8) {
    const size_t quads = CPDF_Annot::QuadPointCount(points.Get());
    for (size_t i = 0; i < quads; ++i) {
      Quad corners;
      for (size_t c = 0; c < 4; ++c) {
        corners[c] = CFX_PointF(points->GetFloatAt(i * 8 + c * 2),
                                points->GetFloatAt(i * 8 + c * 2 + 1));
      }
      std::optional<RedactRegion> region = RedactRegionFromQuadCorners(corners);
      if (!region.has_value() || region->bbox.IsEmpty()) {
        regions.clear();
        break;
      }
      regions.push_back(*region);
    }
    if (!regions.empty()) {
      return regions;
    }
  }
  CFX_FloatRect rect = annot->GetRectFor("Rect");
  rect.Normalize();
  if (!rect.IsEmpty()) {
    RedactRegion region;
    region.bbox = rect;
    regions.push_back(region);
  }
  return regions;
}

Entry BoxEntry(const char* kind, const CFX_FloatRect& box, size_t before) {
  Entry entry;
  entry.kind = kind;
  entry.rect = box;
  entry.before = before;
  RedactRegion region;
  region.bbox = box;
  entry.regions.push_back(region);
  return entry;
}

// Filled boxes in the content, forms included. Overlay tags are not trusted
// here: a file could hide content behind one.
void FindBoxes(CPDF_PageObjectHolder* holder,
               const CFX_Matrix& to_page,
               int depth,
               Context* context,
               std::vector<Entry>* entries) {
  if (depth > kFormLevelLimit) {
    context->failed = true;
    return;
  }
  for (auto it = holder->begin(); it != holder->end(); ++it) {
    CPDF_PageObject* object = it->get();
    if (CPDF_PathObject* path = object->AsPath()) {
      std::optional<CFX_FloatRect> box = OpaqueFilledBox(*path, to_page);
      if (!box.has_value() || box->Width() < kBoxMinSize ||
          box->Height() < kBoxMinSize) {
        continue;
      }
      std::optional<float> luminance = Luminance(*path);
      if (luminance.has_value() &&
          (*luminance < kBoxDarkBelow || *luminance > kBoxLightAbove)) {
        Entry entry =
            BoxEntry("filled-box", *box, context->index.order[object]);
        entry.own_object = path;
        entries->push_back(std::move(entry));
      }
    } else if (CPDF_FormObject* form_object = object->AsForm()) {
      if (CPDF_Form* form = form_object->form()) {
        FindBoxes(form, form_object->form_matrix() * to_page, depth + 1,
                  context, entries);
      }
    }
  }
}

// The box of an annotation drawn as one opaque filled rectangle.
std::optional<CFX_FloatRect> AnnotationBox(CPDF_Page* page,
                                           const CPDF_Dictionary* annot) {
  if (annot->KeyExist("CA") && annot->GetFloatFor("CA") < 1.0f) {
    return std::nullopt;
  }
  RetainPtr<CPDF_Stream> stream =
      GetAnnotAP(annot, CPDF_Annot::AppearanceMode::kNormal);
  if (!stream) {
    return std::nullopt;
  }
  CFX_Matrix to_page;
  std::unique_ptr<CPDF_Form> form =
      Appearance(page, annot, std::move(stream), &to_page);
  if (form->GetPageObjectCount() != 1) {
    return std::nullopt;
  }
  CPDF_PathObject* path = form->GetPageObjectByIndex(0)->AsPath();
  return path ? OpaqueFilledBox(*path, to_page) : std::nullopt;
}

std::vector<Entry> FindCandidates(Context* context) {
  CPDF_Page* page = context->page;
  const size_t content = context->index.content_count;
  std::vector<Entry> entries;
  RetainPtr<const CPDF_Array> annots = page->GetAnnotsArray();
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    if (!annot || annot->GetNameFor("Subtype") != "Redact") {
      continue;
    }
    Entry entry;
    entry.kind = "redact-annotation";
    entry.regions = RegionsOfRedact(annot.Get());
    if (entry.regions.empty()) {
      continue;
    }
    entry.rect = entry.regions[0].bbox;
    for (const RedactRegion& region : entry.regions) {
      entry.rect.Union(region.bbox);
    }
    entry.before = content + i;
    entry.own_annot = annot.Get();
    entries.push_back(std::move(entry));
  }
  FindBoxes(page, CFX_Matrix(), 0, context, &entries);
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    const ByteString subtype = annot ? annot->GetNameFor("Subtype") : "";
    if (subtype != "Square" && subtype != "Highlight" && subtype != "Stamp") {
      continue;
    }
    std::optional<CFX_FloatRect> box = AnnotationBox(page, annot.Get());
    if (box.has_value()) {
      Entry entry = BoxEntry("annotation-box", *box, content + i);
      entry.own_annot = annot.Get();
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

// Holds |entry|'s regions to the page and shrinks them for the tests.
// Returns the path redactor over them, or null: with the context's |failed|
// set when it could not be made, without when no region is left.
std::unique_ptr<CPDF_PathRedactor> Prepare(Context* context, Entry* entry) {
  CPDF_Page* page = context->page;
  // Regions are held to the page, grown by a margin: a mark's quads come from
  // the file and may be any size. One wholly outside has nothing under it;
  // one partly outside becomes its box cut to the page.
  const CFX_SizeF size = page->GetPageSize();
  CFX_FloatRect page_box =
      page->GetPageMatrix().GetInverse().TransformRect(
          CFX_FloatRect(0, 0, size.width, size.height));
  page_box.Normalize();
  page_box.Inflate(kPageMargin, kPageMargin);
  if (!Finite(page_box)) {
    // No region can be held to it, and none may be dropped for missing it.
    context->failed = true;
    return nullptr;
  }
  std::vector<RedactRegion> held;
  for (const RedactRegion& region : entry->regions) {
    if (!Intersects(region.bbox, page_box)) {
      continue;
    }
    if (page_box.Contains(region.bbox)) {
      held.push_back(region);
      continue;
    }
    RedactRegion cut;
    cut.bbox = region.bbox;
    cut.bbox.Intersect(page_box);
    held.push_back(cut);
  }
  entry->regions = std::move(held);
  if (entry->regions.empty()) {
    return nullptr;
  }
  // A region too small to shrink is tested as it is.
  std::vector<RedactRegion>& inset = entry->inset;
  for (const RedactRegion& region : entry->regions) {
    inset.push_back(Inset(region, kPathInset).value_or(region));
    entry->second.push_back(
        Inset(region, SecondTestInset(region)).value_or(region));
  }
  auto redactor = std::make_unique<CPDF_PathRedactor>(pdfium::span(inset));
  if (!redactor->IsValid()) {
    context->failed = true;
    return nullptr;
  }
  return redactor;
}

// What lies under |entry|'s regions that the apply cannot rewrite, on the
// page content and in annotation appearances. Sets the context's |failed|
// when it could not look everywhere.
// Charges the page's own content, already parsed, to the work budget: its
// content streams' bytes as the file holds them, and its objects.
void ChargePage(Context* context) {
  CPDF_Page* page = context->page;
  uint64_t units = page->GetPageObjectCount() + 1;
  RetainPtr<const CPDF_Object> contents =
      page->GetDict()->GetDirectObjectFor("Contents");
  if (const CPDF_Stream* stream = contents ? contents->AsStream() : nullptr) {
    units += stream->GetRawSize();
  } else if (const CPDF_Array* streams = contents ? contents->AsArray() : nullptr) {
    for (size_t i = 0; i < streams->size(); ++i) {
      RetainPtr<const CPDF_Stream> part = streams->GetStreamAt(i);
      units += part ? part->GetRawSize() : 0;
    }
  }
  context->work.Spend(units);
}

// Whether anything the apply cannot rewrite is painted under |entries|'
// regions of |page|: 1 when so, 0 when not, -1 when it could not look.
int UnremovableUnder(CPDF_Page* page, std::vector<Entry>* entries) {
  Context context(page, /*strict=*/true);
  ChargePage(&context);
  // Annotation appearances are not read. The apply removes every annotation
  // whose /Rect meets a region, and one whose /Rect does not is drawn inside
  // that rectangle and cannot paint in the region.
  for (Entry& entry : *entries) {
    std::unique_ptr<CPDF_PathRedactor> redactor = Prepare(&context, &entry);
    if (redactor) {
      Walker walker(&entry, redactor.get(), &context);
      walker.Unremovable(page, CFX_Matrix(), 0, Clips(), nullptr);
    }
    if (context.failed || context.work.spent) {
      return -1;
    }
  }
  for (const Entry& entry : *entries) {
    if (entry.patterns || entry.shadings || entry.mask_groups || entry.glyphs) {
      return 1;
    }
  }
  return 0;
}

// Fills in |entry|'s counts. Sets the context's |failed| when it could not
// look everywhere.
void Inspect(Context* context, Entry* entry) {
  std::unique_ptr<CPDF_PathRedactor> redactor = Prepare(context, entry);
  if (!redactor) {
    return;
  }
  CPDF_Page* page = context->page;
  Walker walker(entry, redactor.get(), context);
  walker.Holder(page, CFX_Matrix(), Scope::kPage, true, 0, Clips(), nullptr);
  if (context->work.spent) {
    context->failed = true;
    return;
  }
  entry->counts.text_chars_second += ExtractedChars(context, *entry);

  const size_t content = context->index.content_count;
  RetainPtr<const CPDF_Array> annots = page->GetAnnotsArray();
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    if (content + i >= entry->before) {
      break;
    }
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    if (!annot || annot.Get() == entry->own_annot) {
      continue;
    }
    const ByteString subtype = annot->GetNameFor("Subtype");
    if (subtype == "Redact" || subtype == "Popup") {
      continue;
    }
    CFX_FloatRect rect = annot->GetRectFor("Rect");
    bool counted = false;
    for (const RedactRegion& region : entry->regions) {
      counted |= RedactRegionIntersectsRect(region, rect);
    }
    for (RetainPtr<CPDF_Stream>& stream : AppearanceStreams(annot.Get())) {
      bool fell_back = false;
      ChargeStream(&context->work, stream.Get(), &fell_back);
      if (context->work.spent) {
        break;
      }
      CFX_Matrix to_page;
      std::unique_ptr<CPDF_Form> form =
          Appearance(page, annot.Get(), std::move(stream), &to_page);
      // An appearance that could not be read counts when it lies there.
      if (Unreadable(fell_back, *form)) {
        entry->counts.annotation_text += counted ? 1 : 0;
        continue;
      }
      // Walked either way, for the page copies inside.
      walker.Holder(form.get(), to_page, Scope::kAnnotation, counted, 0,
                    Clips(), nullptr);
    }
  }
  // A region counts content when either test finds it.
  Counts& counts = entry->counts;
  counts.text_chars = std::max(counts.text_chars, counts.text_chars_second);
  counts.annotation_text =
      std::max(counts.annotation_text, counts.annotation_text_second);
}

// -- Reports -----------------------------------------------------------------

void AppendNumber(std::string* out, float value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.6g", std::isfinite(value) ? value : 0.0f);
  *out += buffer;
}

void AppendField(std::string* out, const char* key, unsigned value) {
  *out += ",\"";
  *out += key;
  *out += "\":";
  *out += std::to_string(value);
}

char* Copy(const std::string& text) {
  char* report = static_cast<char*>(malloc(text.size() + 1));
  if (report) {
    memcpy(report, text.c_str(), text.size() + 1);
  }
  return report;
}

std::string PageReport(const std::vector<Entry>& entries) {
  std::string out = "{\"regions\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    out += i ? ",{" : "{";
    out += "\"index\":" + std::to_string(i) + ",\"rect\":[";
    AppendNumber(&out, e.rect.left);
    out += ",";
    AppendNumber(&out, e.rect.bottom);
    out += ",";
    AppendNumber(&out, e.rect.right);
    out += ",";
    AppendNumber(&out, e.rect.top);
    out += "],\"kind\":\"";
    out += e.kind;
    out += "\"";
    AppendField(&out, "textChars", e.counts.text_chars);
    AppendField(&out, "imagesNotBlank", e.counts.images_not_blank);
    AppendField(&out, "paths", e.counts.paths);
    AppendField(&out, "patternContent", e.counts.pattern_content);
    AppendField(&out, "annotationText", e.counts.annotation_text);
    AppendField(&out, "annotationImages", e.counts.annotation_images);
    out += "}";
  }
  out += "]}";
  return out;
}

std::string UnremovableReport(const std::vector<Entry>& entries) {
  std::string out = "{\"regions\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    out += i ? ",{" : "{";
    out += "\"index\":" + std::to_string(i);
    AppendField(&out, "patterns", e.patterns);
    AppendField(&out, "shadings", e.shadings);
    AppendField(&out, "maskGroups", e.mask_groups);
    AppendField(&out, "glyphs", e.glyphs);
    out += "}";
  }
  out += "]}";
  return out;
}

// One entry for each of |count| quads of 8 floats at |quads|, or nullopt
// when one is not a region.
std::optional<std::vector<Entry>> EntriesOf(const float* quads, int count) {
  std::vector<Entry> entries;
  // SAFETY: the caller passes |count| quads of 8 floats at |quads|.
  pdfium::span<const float> values =
      UNSAFE_BUFFERS(pdfium::span(quads, static_cast<size_t>(count) * 8));
  for (int i = 0; i < count; ++i) {
    Quad corners;
    for (size_t c = 0; c < 4; ++c) {
      corners[c] = CFX_PointF(values[i * 8 + c * 2], values[i * 8 + c * 2 + 1]);
    }
    std::optional<RedactRegion> region = RedactRegionFromQuadCorners(corners);
    if (!region.has_value()) {
      return std::nullopt;
    }
    Entry entry;
    entry.rect = region->bbox;
    entry.regions.push_back(*region);
    entries.push_back(std::move(entry));
  }
  return entries;
}

// The /L of a file's first object when that object is a linearization
// dictionary (its key is exactly /Linearized), read as PDFium's parser reads
// it: the object after the header. Nullopt otherwise.
std::optional<size_t> LinearizedLength(pdfium::span<const uint8_t> bytes) {
  // The offset PDFium's own linearization check reads from.
  constexpr FX_FILESIZE kFirstObjectOffset = 9;
  if (bytes.size() <= static_cast<size_t>(kFirstObjectOffset)) {
    return std::nullopt;
  }
  CPDF_SyntaxParser parser(pdfium::MakeRetain<CFX_ReadOnlySpanStream>(bytes));
  parser.SetPos(kFirstObjectOffset);
  RetainPtr<const CPDF_Dictionary> dict = ToDictionary(
      parser.GetIndirectObject(nullptr, CPDF_SyntaxParser::ParseType::kLoose));
  if (!dict || !dict->KeyExist("Linearized")) {
    return std::nullopt;
  }
  RetainPtr<const CPDF_Number> length = dict->GetNumberFor("L");
  if (!length || !length->IsInteger() || length->GetInteger() <= 0 ||
      static_cast<size_t>(length->GetInteger()) > bytes.size()) {
    return std::nullopt;
  }
  return static_cast<size_t>(length->GetInteger());
}

// Whether the trailer that the first %%EOF (at |first_end|) closes has a
// /Prev pointing forward, past that %%EOF: in a linearized file the first-page
// trailer points on to the main cross-reference table, while an update's
// trailer points back to an earlier one.
bool FirstPrevPointsForward(pdfium::span<const uint8_t> bytes,
                            std::string_view text,
                            size_t first_end) {
  // The first-page section's startxref is 0 in a linearized file, so the
  // trailer is found by its place: the last "trailer" before the first
  // %%EOF, or else a cross-reference stream as the object right after the
  // linearization dictionary.
  CPDF_SyntaxParser parser(pdfium::MakeRetain<CFX_ReadOnlySpanStream>(bytes));
  RetainPtr<const CPDF_Dictionary> trailer;
  const std::string_view head = text.substr(0, first_end);
  const size_t word = head.rfind("trailer");
  if (word != std::string_view::npos) {
    parser.SetPos(static_cast<FX_FILESIZE>(word + 7));
    trailer = ToDictionary(parser.GetObjectBody(nullptr));
  } else {
    const size_t first_object_end = head.find("endobj");
    if (first_object_end == std::string_view::npos) {
      return false;
    }
    parser.SetPos(static_cast<FX_FILESIZE>(first_object_end + 6));
    RetainPtr<const CPDF_Stream> stream = ToStream(
        parser.GetIndirectObject(nullptr, CPDF_SyntaxParser::ParseType::kLoose));
    if (stream && stream->GetDict()->GetNameFor("Type") == "XRef") {
      trailer = stream->GetDict();
    }
  }
  RetainPtr<const CPDF_Number> prev =
      trailer ? trailer->GetNumberFor("Prev") : nullptr;
  return prev && prev->IsInteger() && prev->GetInteger() > 0 &&
         static_cast<size_t>(prev->GetInteger()) > first_end;
}

// One more than the %%EOF markers a further startxref follows. A file that
// is really linearized ends its first-page section with a %%EOF of its own,
// which is not an earlier version and is not counted. Really linearized: its
// first object is a linearization dictionary whose /L is the offset just
// past a %%EOF (and its line end), the one that ends the linearized part; the
// first-page %%EOF is one before that, and the trailer it closes points
// forward with /Prev. Anything else counts.
unsigned Revisions(pdfium::span<const uint8_t> bytes) {
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size());
  std::vector<size_t> ends;  // Offsets of each %%EOF.
  for (size_t at = text.find("%%EOF"); at != std::string_view::npos;
       at = text.find("%%EOF", at + 5)) {
    ends.push_back(at);
  }
  // The %%EOF that ends the linearized part, when the file is linearized.
  std::optional<size_t> part_end;
  if (std::optional<size_t> length = LinearizedLength(bytes)) {
    for (size_t i = 0; i < ends.size(); ++i) {
      size_t past = ends[i] + 5;
      for (int k = 0; k < 2 && past < text.size() &&
                      (text[past] == '\r' || text[past] == '\n');
           ++k) {
        if (past == *length) {
          break;
        }
        ++past;
      }
      if (past == *length) {
        part_end = i;
        break;
      }
    }
  }
  unsigned revisions = 1;
  for (size_t i = 0; i < ends.size(); ++i) {
    if (text.find("startxref", ends[i] + 5) == std::string_view::npos) {
      continue;
    }
    // The first-page section's %%EOF: the first, before the part's end,
    // closing a trailer whose /Prev points forward.
    if (part_end.has_value() && i == 0 && *part_end > 0 &&
        FirstPrevPointsForward(bytes, text, ends[0])) {
      continue;
    }
    ++revisions;
  }
  return revisions;
}

bool HiddenLayers(const CPDF_Dictionary* root) {
  RetainPtr<const CPDF_Dictionary> properties =
      root->GetDictFor("OCProperties");
  RetainPtr<const CPDF_Dictionary> config =
      properties ? properties->GetDictFor("D") : nullptr;
  if (!config) {
    return false;
  }
  RetainPtr<const CPDF_Array> off = config->GetArrayFor("OFF");
  if (off && !off->IsEmpty()) {
    return true;
  }
  RetainPtr<const CPDF_Array> groups = properties->GetArrayFor("OCGs");
  return config->GetNameFor("BaseState") == "OFF" && groups &&
         !groups->IsEmpty();
}

// Whether a field in the tree under |fields| is a signature field with a
// value. Each field is read once, so a tree that loops still ends.
bool Signed(RetainPtr<const CPDF_Array> fields) {
  std::vector<RetainPtr<const CPDF_Array>> pending;
  std::set<const CPDF_Dictionary*> seen;
  if (fields) {
    pending.push_back(std::move(fields));
  }
  while (!pending.empty()) {
    RetainPtr<const CPDF_Array> array = std::move(pending.back());
    pending.pop_back();
    for (size_t i = 0; i < array->size(); ++i) {
      RetainPtr<const CPDF_Dictionary> field = array->GetDictAt(i);
      if (!field || !seen.insert(field.Get()).second) {
        continue;
      }
      if (field->GetNameFor("FT") == "Sig" && field->GetDictFor("V")) {
        return true;
      }
      if (RetainPtr<const CPDF_Array> kids = field->GetArrayFor("Kids")) {
        pending.push_back(std::move(kids));
      }
    }
  }
  return false;
}

}  // namespace

namespace inkbar {

int UnremovableUnderQuads(CPDF_Page* page,
                          const std::vector<std::array<float, 8>>& quads) {
  if (quads.empty()) {
    return 0;
  }
  std::optional<std::vector<Entry>> entries =
      EntriesOf(quads[0].data(), static_cast<int>(quads.size()));
  if (!entries.has_value()) {
    return -1;
  }
  return UnremovableUnder(page, &*entries);
}

}  // namespace inkbar

extern "C" {

char* INK_Inspect_Page(FPDF_DOCUMENT doc,
                       int page_index,
                       const float* quads,
                       int count) {
  if (count < 0 || (count > 0 && !quads)) {
    return nullptr;
  }
  std::vector<Entry> entries;
  if (count > 0) {
    std::optional<std::vector<Entry>> given = EntriesOf(quads, count);
    if (!given.has_value()) {
      return nullptr;
    }
    entries = std::move(*given);
  }

  FPDF_PAGE handle = FPDF_LoadPage(doc, page_index);
  if (!handle) {
    return nullptr;
  }
  std::string report;
  {
    ScopedFPDFPageView view(handle);
    Context context(view.Get(), /*strict=*/count > 0);
    ChargePage(&context);
    IndexHolder(context.page, false, 0, &context);
    if (count == 0) {
      entries = FindCandidates(&context);
    }
    for (Entry& entry : entries) {
      Inspect(&context, &entry);
      if (context.work.spent) {
        // Out of work: the rest goes unread, and the call fails.
        context.failed = true;
        break;
      }
    }
    if (!context.failed) {
      report = PageReport(entries);
    }
  }
  FPDF_ClosePage(handle);
  return report.empty() ? nullptr : Copy(report);
}

char* INK_Redact_Unremovable(FPDF_DOCUMENT doc,
                             int page_index,
                             const float* quads,
                             int count) {
  if (count <= 0 || !quads) {
    return nullptr;
  }
  std::optional<std::vector<Entry>> entries = EntriesOf(quads, count);
  if (!entries.has_value()) {
    return nullptr;
  }
  FPDF_PAGE handle = FPDF_LoadPage(doc, page_index);
  if (!handle) {
    return nullptr;
  }
  std::string report;
  {
    ScopedFPDFPageView view(handle);
    if (UnremovableUnder(view.Get(), &*entries) >= 0) {
      report = UnremovableReport(*entries);
    }
  }
  FPDF_ClosePage(handle);
  return report.empty() ? nullptr : Copy(report);
}

char* INK_Inspect_File(FPDF_DOCUMENT doc,
                       const unsigned char* bytes,
                       unsigned long size) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || (!bytes && size)) {
    return nullptr;
  }
  const CPDF_Dictionary* root = pdf->GetRoot();
  if (!root) {
    return nullptr;
  }
  // SAFETY: the caller passes |size| readable bytes at |bytes|.
  const unsigned revisions =
      size ? Revisions(UNSAFE_BUFFERS(pdfium::span(bytes, size))) : 1;
  RetainPtr<const CPDF_Dictionary> info = pdf->GetInfo();
  unsigned attachments = 0;
  const unsigned found = inkbar::FindSanitizable(pdf, &attachments);
  RetainPtr<const CPDF_Dictionary> form = root->GetDictFor("AcroForm");

  auto boolean = [](bool value) { return value ? "true" : "false"; };
  std::string out = "{\"revisions\":" + std::to_string(revisions);
  out += ",\"info\":";
  // The trailer's /Info may be a direct dictionary, which GetInfo() does not
  // read.
  const CPDF_Parser* parser = pdf->GetParser();
  const CPDF_Dictionary* trailer = parser ? parser->GetTrailer() : nullptr;
  RetainPtr<const CPDF_Dictionary> trailer_info =
      trailer ? trailer->GetDictFor("Info") : nullptr;
  out += boolean((info && info->size() > 0) ||
                 (trailer_info && trailer_info->size() > 0));
  out += ",\"xmp\":";
  out += boolean(!!root->GetStreamFor("Metadata"));
  out += ",\"attachments\":" + std::to_string(attachments);
  out += ",\"scripts\":";
  out += boolean(found & INK_SANITIZE_SCRIPTS);
  out += ",\"hiddenLayers\":";
  out += boolean(HiddenLayers(root));
  out += ",\"signed\":";
  out += boolean(form && Signed(form->GetArrayFor("Fields")));
  out += "}";
  return Copy(out);
}

void INK_Inspect_Free(char* report) {
  free(report);
}

}  // extern "C"
