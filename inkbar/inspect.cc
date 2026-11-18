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
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/fpdfapi/edit/cpdf_path_redactor.h"
#include "core/fpdfapi/edit/cpdf_text_redactor.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorstate.h"
#include "core/fpdfapi/page/cpdf_dib.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_generalstate.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_pattern.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_tilingpattern.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdftext/cpdf_textpage.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxge/dib/cfx_dibbase.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
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
// After DCT or JPX, samples this close to the edge of the blanked area may
// stray twice as far: the apply re-encodes the 8 x 8 blocks the edge crosses
// with the kept content beside the hole, and that content rings into it.
constexpr int kLossyEdge = 8;
// Filled boxes that count as candidates in the page content.
constexpr float kBoxMinSize = 4.0f;
constexpr float kBoxDarkBelow = 0.2f;
constexpr float kBoxLightAbove = 0.98f;
// PDFium's content parser leaves a form nested deeper than this unparsed and
// empty (kMaxFormLevel in cpdf_streamcontentparser.cpp), though another
// viewer may draw it. Meeting one fails the inspection.
constexpr int kFormLevelLimit = 40;
// Patterns painted with patterns, more deeply nested than this, fail the
// inspection rather than going unread.
constexpr int kMaxPatternChain = 64;

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

// |region| shrunk by |amount| on every side, or nullopt when nothing is
// left. A quad's corners each move |amount| toward its centre.
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
  CFX_PointF centre;
  for (const CFX_PointF& p : region.quad) {
    centre += p;
  }
  centre = CFX_PointF(centre.x / 4, centre.y / 4);
  Quad corners;
  for (size_t i = 0; i < 4; ++i) {
    const CFX_PointF to_centre = centre - region.quad[i];
    const float length = std::hypot(to_centre.x, to_centre.y);
    if (length <= 2 * amount) {
      return std::nullopt;
    }
    const float step = amount / length;
    corners[i] = CFX_PointF(region.quad[i].x + to_centre.x * step,
                            region.quad[i].y + to_centre.y * step);
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
};

class SampleCache {
 public:
  explicit SampleCache(CPDF_Document* doc) : doc_(doc) {}

  const Samples& Get(RetainPtr<const CPDF_Stream> stream) {
    auto it = cache_.find(stream.Get());
    if (it != cache_.end()) {
      return it->second;
    }
    Samples samples;
    samples.tolerance = Tolerance(stream->GetDict().Get());
    samples.dib = Decode(stream);
    // The cache holds the stream, so its address names it for the call.
    streams_.push_back(stream);
    return cache_.emplace(stream.Get(), std::move(samples)).first->second;
  }

 private:
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

  CPDF_Document* const doc_;
  std::vector<RetainPtr<const CPDF_Stream>> streams_;
  std::map<const CPDF_Stream*, Samples> cache_;
};

// Whether the samples of |samples| over the regions, on its own grid, are
// one value. Samples that could not be decoded are not.
bool SamplesFlat(const Samples& samples,
                 const CFX_Matrix& image_to_page,
                 const std::vector<RedactRegion>& regions) {
  const CFX_DIBBase* dib = samples.dib.Get();
  if (!dib) {
    return false;
  }
  const int width = dib->GetWidth();
  const int height = dib->GetHeight();
  const std::vector<GridBox> boxes =
      GridBoxes(image_to_page, width, height, regions);
  // |band| holds every sample; |inner| those away from a lossy edge.
  const bool lossy = samples.tolerance > 0;
  Band band(lossy ? 2 * samples.tolerance : 0);
  Band inner(samples.tolerance);
  const bool mask = dib->IsMaskFormat();
  const bool one_bit = dib->GetBPP() == 1;
  const size_t bytes = mask ? 1 : dib->GetBPP() / 8;
  for (const GridBox& b : boxes) {
    for (int y = b.y0; y < b.y1 && band.flat() && inner.flat(); ++y) {
      pdfium::span<const uint8_t> row = dib->GetScanline(height - 1 - y);
      for (int x = b.x0; x < b.x1; ++x) {
        const bool edge =
            lossy && std::min({x - b.x0, b.x1 - 1 - x, y - b.y0,
                               b.y1 - 1 - y}) < kLossyEdge;
        if (mask) {
          const size_t at = one_bit ? x / 8 : x;
          if (at >= row.size()) {
            return false;
          }
          const int value = one_bit ? (row[at] >> (7 - x % 8)) & 1 : row[at];
          band.Add(0, value);
          if (!edge) {
            inner.Add(0, value);
          }
          continue;
        }
        const size_t at = static_cast<size_t>(x) * bytes;
        if (at + 2 >= row.size()) {
          return false;
        }
        for (int c = 0; c < 3; ++c) {
          band.Add(c, row[at + c]);
          if (!edge) {
            inner.Add(c, row[at + c]);
          }
        }
      }
    }
  }
  return band.flat() && inner.flat();
}

// Whether an image is blank under the regions: its own samples, and those of
// its /SMask and of a stream /Mask, each on its own grid.
bool ImageBlank(SampleCache* cache,
                const CPDF_ImageObject* object,
                const CFX_Matrix& to_page,
                const std::vector<RedactRegion>& regions) {
  RetainPtr<CPDF_Image> image = object->GetImage();
  RetainPtr<const CPDF_Stream> stream = image ? image->GetStream() : nullptr;
  if (!stream) {
    return false;
  }
  const CFX_Matrix image_to_page = object->matrix() * to_page;
  if (!SamplesFlat(cache->Get(stream), image_to_page, regions)) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> dict = stream->GetDict();
  for (const char* key : {"SMask", "Mask"}) {
    RetainPtr<const CPDF_Stream> mask = dict->GetStreamFor(key);
    if (mask && !SamplesFlat(cache->Get(mask), image_to_page, regions)) {
      return false;
    }
  }
  return true;
}

// -- Paths -------------------------------------------------------------------

// A copy of |source| to hand to PDFium's path redactor, which cuts what it is
// given: the redactor says whether it would cut the original.
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
      : page(page), samples(page->GetDocument()), strict(strict) {}

  CPDF_Page* const page;
  SampleCache samples;
  PageIndex index;
  std::unique_ptr<CPDF_TextPage> text_page;  // Made on first use.
  // Regions given: forms tagged /INKB_Overlay are an apply's own drawing.
  // Without regions the tag is not trusted.
  const bool strict;
  bool failed = false;
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

// The points of a path in page space, its segments sampled at most 1 pt
// apart, for |visit|. Stops when |visit| returns true; returns whether it did.
template <typename Visit>
bool SamplePath(const CPDF_Path& path, const CFX_Matrix& to_page, Visit visit) {
  const std::vector<CFX_Path::Point>& points = path.GetPoints();
  auto line = [&](const CFX_PointF& a, const CFX_PointF& b) {
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    const int steps = std::clamp(static_cast<int>(std::ceil(length)), 1,
                                 1 << 20);
    for (int i = 0; i <= steps; ++i) {
      const float t = static_cast<float>(i) / steps;
      if (visit(CFX_PointF(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t))) {
        return true;
      }
    }
    return false;
  };
  CFX_PointF start;
  CFX_PointF current;
  for (size_t i = 0; i < points.size(); ++i) {
    const CFX_Path::Point& point = points[i];
    const CFX_PointF p = to_page.Transform(point.point_);
    switch (point.type_) {
      case CFX_Path::Point::Type::kMove:
        start = current = p;
        if (visit(p)) {
          return true;
        }
        break;
      case CFX_Path::Point::Type::kLine:
        if (line(current, p)) {
          return true;
        }
        current = p;
        break;
      case CFX_Path::Point::Type::kBezier: {
        if (i + 2 >= points.size()) {
          return false;
        }
        const CFX_PointF c1 = p;
        const CFX_PointF c2 = to_page.Transform(points[i + 1].point_);
        const CFX_PointF end = to_page.Transform(points[i + 2].point_);
        // The control polygon is never shorter than the curve.
        const float length = std::hypot(c1.x - current.x, c1.y - current.y) +
                             std::hypot(c2.x - c1.x, c2.y - c1.y) +
                             std::hypot(end.x - c2.x, end.y - c2.y);
        const int steps = std::clamp(static_cast<int>(std::ceil(length)), 1,
                                     1 << 20);
        CFX_PointF previous = current;
        for (int k = 1; k <= steps; ++k) {
          const float t = static_cast<float>(k) / steps;
          const float u = 1 - t;
          const CFX_PointF q(
              u * u * u * current.x + 3 * u * u * t * c1.x +
                  3 * u * t * t * c2.x + t * t * t * end.x,
              u * u * u * current.y + 3 * u * u * t * c1.y +
                  3 * u * t * t * c2.y + t * t * t * end.y);
          if (line(previous, q)) {
            return true;
          }
          previous = q;
        }
        current = end;
        i += 2;
        if (points[i].close_figure_ && line(current, start)) {
          return true;
        }
        continue;
      }
    }
    if (point.close_figure_ && line(current, start)) {
      return true;
    }
  }
  return false;
}

class Walker {
 public:
  Walker(Entry* entry, const CPDF_PathRedactor* redactor, Context* context)
      : entry_(entry), redactor_(redactor), context_(context) {}

  void Holder(CPDF_PageObjectHolder* holder,
              const CFX_Matrix& to_page,
              Scope scope,
              bool counted,
              int depth) {
    if (depth > kFormLevelLimit) {
      context_->failed = true;
      return;
    }
    const bool page = scope == Scope::kPage;
    for (auto it = holder->begin(); it != holder->end(); ++it) {
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
                 Scope::kPageCopy, true, depth + 1);
          continue;
        }
        Holder(form, form_object->form_matrix() * to_page, scope, counted,
               depth + 1);
        continue;
      }
      if (page && !Before(object)) {
        continue;
      }
      if (counted) {
        Patterns(object, to_page);
      }
      if (CPDF_TextObject* text = object->AsText()) {
        if (counted) {
          const unsigned chars = TextChars(*text, to_page);
          if (page) {
            entry_->counts.text_chars += chars;
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
        if (page && (PathCrosses(*redactor_, *path, to_page) ||
                     SamplePath(path->path(), path->matrix() * to_page,
                                [this](const CFX_PointF& p) {
                                  return PointInsideAny(p, entry_->second);
                                }))) {
          ++entry_->counts.paths;
        }
      } else if (CPDF_ShadingObject* shading = object->AsShading()) {
        if (page && ShadingCrosses(*redactor_, *shading, to_page)) {
          ++entry_->counts.paths;
        }
      }
    }
  }

 private:
  bool Before(const CPDF_PageObject* object) const {
    auto it = context_->index.order.find(object);
    return it == context_->index.order.end() || it->second < entry_->before;
  }

  // Text and images in the content streams of tiling patterns an object
  // paints into the regions with.
  void Patterns(CPDF_PageObject* object, const CFX_Matrix& to_page) {
    CFX_FloatRect box = to_page.TransformRect(object->GetRect());
    box.Normalize();
    if (!RectMeetsAnyBBox(box, entry_->regions)) {
      return;
    }
    std::set<const CPDF_Object*> chain;
    entry_->counts.pattern_content += PatternsOf(object, &chain);
  }

  // The characters and images inside the tiling patterns |object| paints
  // with, following patterns those paint with in turn. |chain| holds the
  // patterns being read, so a pattern that paints with itself ends.
  unsigned PatternsOf(CPDF_PageObject* object,
                      std::set<const CPDF_Object*>* chain) {
    const CPDF_ColorState& colors = object->color_state();
    if (!colors.HasRef()) {
      return 0;
    }
    unsigned count = 0;
    for (const CPDF_Color* color :
         {colors.GetFillColor(), colors.GetStrokeColor()}) {
      if (!color || !color->IsPattern()) {
        continue;
      }
      RetainPtr<CPDF_Pattern> pattern = color->GetPattern();
      CPDF_TilingPattern* tiling = pattern ? pattern->AsTilingPattern() : nullptr;
      if (!tiling || chain->count(tiling->pattern_obj().Get())) {
        continue;
      }
      if (chain->size() >= kMaxPatternChain) {
        context_->failed = true;
        return count;
      }
      chain->insert(tiling->pattern_obj().Get());
      // A pattern's content is parsed on its own, from level 0.
      std::unique_ptr<CPDF_Form> form = tiling->Load(object);
      if (form) {
        count += PatternContent(form.get(), chain, 0);
      }
      chain->erase(tiling->pattern_obj().Get());
    }
    return count;
  }

  unsigned PatternContent(CPDF_PageObjectHolder* holder,
                          std::set<const CPDF_Object*>* chain,
                          int depth) {
    if (depth > kFormLevelLimit) {
      context_->failed = true;
      return 0;
    }
    unsigned count = 0;
    for (auto it = holder->begin(); it != holder->end(); ++it) {
      CPDF_PageObject* object = it->get();
      count += PatternsOf(object, chain);
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
    return !ImageBlank(&context_->samples, &image, to_page, entry_->regions);
  }

  Entry* const entry_;
  const CPDF_PathRedactor* const redactor_;
  Context* const context_;
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

// Fills in |entry|'s counts. Sets the context's |failed| when it could not
// look everywhere.
void Inspect(Context* context, Entry* entry) {
  CPDF_Page* page = context->page;
  // A region too small to shrink is tested as it is.
  std::vector<RedactRegion> inset;
  for (const RedactRegion& region : entry->regions) {
    inset.push_back(Inset(region, kPathInset).value_or(region));
    entry->second.push_back(
        Inset(region, SecondTestInset(region)).value_or(region));
  }
  auto redactor = std::make_unique<CPDF_PathRedactor>(pdfium::span(inset));
  if (!redactor->IsValid()) {
    context->failed = true;
    return;
  }
  Walker walker(entry, redactor.get(), context);
  walker.Holder(page, CFX_Matrix(), Scope::kPage, true, 0);
  entry->counts.text_chars_second = ExtractedChars(context, *entry);

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
      CFX_Matrix to_page;
      std::unique_ptr<CPDF_Form> form =
          Appearance(page, annot.Get(), std::move(stream), &to_page);
      // Walked either way, for the page copies inside.
      walker.Holder(form.get(), to_page, Scope::kAnnotation, counted, 0);
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

// One more than the %%EOF markers a further startxref follows.
unsigned Revisions(pdfium::span<const uint8_t> bytes) {
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size());
  unsigned revisions = 1;
  size_t at = 0;
  while ((at = text.find("%%EOF", at)) != std::string_view::npos) {
    at += 5;
    if (text.find("startxref", at) != std::string_view::npos) {
      ++revisions;
    }
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
        return nullptr;
      }
      Entry entry;
      entry.rect = region->bbox;
      entry.regions.push_back(*region);
      entries.push_back(std::move(entry));
    }
  }

  FPDF_PAGE handle = FPDF_LoadPage(doc, page_index);
  if (!handle) {
    return nullptr;
  }
  std::string report;
  {
    ScopedFPDFPageView view(handle);
    Context context(view.Get(), /*strict=*/count > 0);
    IndexHolder(context.page, false, 0, &context);
    if (count == 0) {
      entries = FindCandidates(&context);
    }
    for (Entry& entry : entries) {
      Inspect(&context, &entry);
    }
    if (!context.failed) {
      report = PageReport(entries);
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
