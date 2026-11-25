// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See render.h. A thin layer over FPDF_RenderPageBitmap_Start,
// FPDF_RenderPage_Continue and FPDF_RenderPage_Close with a pause on a clock.
//
// Leaving out: PDFium lists the annotations it draws, and reads their /F
// flags, inside FPDF_RenderPageBitmap_Start, which builds a fresh annotation
// list for every render and queues each appearance it keeps. So Start sets
// the Hidden flag on the listed annotations, starts the render and puts every
// /F back as it was, the same object or none, before it returns. Nothing
// later in the render reads /F.

#include "inkbar/render.h"

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "constants/annotation_common.h"
#include "constants/annotation_flags.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_progressive.h"

struct InkRender {
  IFSDK_PAUSE pause;
  FPDF_PAGE page;
  // The context Start put on the page. Continue and Close act only while the
  // page holds it and this is the page's latest render.
  const void* context;
  std::chrono::steady_clock::time_point deadline;
  int status;
};

namespace {

// The latest InkRender started on each page. An older one is stale even when
// a newer context took its address.
std::map<FPDF_PAGE, InkRender*>& Latest() {
  static auto* latest = new std::map<FPDF_PAGE, InkRender*>;
  return *latest;
}

const void* ContextOf(FPDF_PAGE page) {
  ScopedFPDFPageView view(page);
  return view ? view.Get()->GetRenderContext() : nullptr;
}

// Whether |r| still owns the render its page holds.
bool Current(const InkRender* r) {
  auto it = Latest().find(r->page);
  return it != Latest().end() && it->second == r &&
         ContextOf(r->page) == r->context;
}

FPDF_BOOL PastDeadline(IFSDK_PAUSE* pause) {
  const auto* r = static_cast<const InkRender*>(pause->user);
  return std::chrono::steady_clock::now() >= r->deadline;
}

// An annotation's /F as it was before Start hid it: the object, or null when
// it had none.
struct SavedFlags {
  RetainPtr<CPDF_Dictionary> dict;
  RetainPtr<CPDF_Object> flags;
};

// Sets the Hidden flag on each annotation of |page| whose object number is in
// |skip| or whose place in /Annots is in |skip_at|, and returns what to put
// back, in the order it was changed.
std::vector<SavedFlags> HideListed(FPDF_PAGE page,
                                   const unsigned int* skip,
                                   int skip_count,
                                   const int* skip_at,
                                   int skip_at_count) {
  std::vector<SavedFlags> saved;
  if (skip_count <= 0 && skip_at_count <= 0) {
    return saved;
  }
  std::unordered_set<uint32_t> listed;
  for (int i = 0; i < skip_count; ++i) {
    if (skip[i] != 0) {
      listed.insert(skip[i]);
    }
  }
  std::unordered_set<size_t> places;
  for (int i = 0; i < skip_at_count; ++i) {
    if (skip_at[i] >= 0) {
      places.insert(static_cast<size_t>(skip_at[i]));
    }
  }
  ScopedFPDFPageView view(page);
  const CPDF_Page* pdf_page = view.Get();
  RetainPtr<const CPDF_Array> annots =
      pdf_page ? pdf_page->GetAnnotsArray() : nullptr;
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> dict = annots->GetDictAt(i);
    // A place names a dictionary only, as the reports count them: a stream
    // there is not an annotation.
    const bool at_place =
        places.count(i) && ToDictionary(annots->GetDirectObjectAt(i));
    if (!dict || (!listed.count(dict->GetObjNum()) && !at_place)) {
      continue;
    }
    // The render lists annotations through the same const_cast.
    RetainPtr<CPDF_Dictionary> mutable_dict =
        pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(dict.Get()));
    RetainPtr<CPDF_Object> flags =
        mutable_dict->GetMutableObjectFor(pdfium::annotation::kF);
    const uint32_t value =
        static_cast<uint32_t>(dict->GetIntegerFor(pdfium::annotation::kF)) |
        pdfium::annotation_flags::kHidden;
    saved.push_back({mutable_dict, std::move(flags)});
    mutable_dict->SetNewFor<CPDF_Number>(pdfium::annotation::kF,
                                         static_cast<int>(value));
  }
  return saved;
}

// Puts back what HideListed changed, last first, so an annotation listed
// twice in /Annots ends with its first saved value.
void Restore(std::vector<SavedFlags>& saved) {
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    if (it->flags) {
      it->dict->SetFor(pdfium::annotation::kF, it->flags);
    } else {
      it->dict->RemoveFor(pdfium::annotation::kF);
    }
  }
  saved.clear();
}

float UserUnitOf(const CPDF_Dictionary* page) {
  const float value = page ? page->GetFloatFor("UserUnit") : 0.0f;
  return std::isfinite(value) && value > 0 ? value : 1.0f;
}

int ToResult(int status) {
  if (status == FPDF_RENDER_DONE) {
    return 1;
  }
  return status == FPDF_RENDER_TOBECONTINUED ? 0 : -1;
}

}  // namespace

extern "C" {

InkRender* INK_Render_Start(FPDF_BITMAP bitmap,
                            FPDF_PAGE page,
                            int start_x,
                            int start_y,
                            int size_x,
                            int size_y,
                            int rotate,
                            int flags,
                            const unsigned int* skip,
                            int skip_count,
                            const int* skip_at,
                            int skip_at_count) {
  if (!bitmap || !page || skip_count < 0 || (skip_count > 0 && !skip) ||
      skip_at_count < 0 || (skip_at_count > 0 && !skip_at)) {
    return nullptr;
  }
  {
    ScopedFPDFPageView view(page);
    if (!view || view.Get()->GetRenderContext()) {
      return nullptr;
    }
  }

  auto r = std::make_unique<InkRender>();
  r->pause.version = 1;
  r->pause.NeedToPauseNow = PastDeadline;
  r->pause.user = r.get();
  r->page = page;
  // Start does the least it can: the caller's slices do the drawing.
  r->deadline = std::chrono::steady_clock::now();

  std::vector<SavedFlags> saved =
      HideListed(page, skip, skip_count, skip_at, skip_at_count);
  r->status = FPDF_RenderPageBitmap_Start(bitmap, page, start_x, start_y,
                                          size_x, size_y, rotate, flags,
                                          &r->pause);
  Restore(saved);
  if (r->status != FPDF_RENDER_TOBECONTINUED &&
      r->status != FPDF_RENDER_DONE) {
    // A failed start may leave its context on the page.
    FPDF_RenderPage_Close(page);
    return nullptr;
  }
  r->context = ContextOf(page);
  Latest()[page] = r.get();
  return r.release();
}

int INK_Render_Continue(InkRender* r, int slice_ms) {
  if (!r) {
    return -1;
  }
  // A render another one replaced fails; a finished one stays finished.
  if (r->status == FPDF_RENDER_TOBECONTINUED && !Current(r)) {
    r->status = FPDF_RENDER_FAILED;
  }
  if (r->status == FPDF_RENDER_TOBECONTINUED) {
    r->deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(slice_ms > 0 ? slice_ms : 0);
    r->status = FPDF_RenderPage_Continue(r->page, &r->pause);
  }
  return ToResult(r->status);
}

void INK_Render_Close(InkRender* r) {
  if (!r) {
    return;
  }
  if (Current(r)) {
    FPDF_RenderPage_Close(r->page);
  }
  auto it = Latest().find(r->page);
  if (it != Latest().end() && it->second == r) {
    Latest().erase(it);
  }
  delete r;
}

float INK_Page_UserUnit(FPDF_PAGE page) {
  ScopedFPDFPageView view(page);
  const CPDF_Page* pdf_page = view.Get();
  return pdf_page ? UserUnitOf(pdf_page->GetDict().Get()) : 1.0f;
}

int INK_Page_Facts(FPDF_DOCUMENT doc, int page_index, float* out) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || !out || page_index < 0 || page_index >= pdf->GetPageCount()) {
    return 0;
  }
  // As FPDF_LoadPage makes the page (NewPage() in fpdf_view.cpp), without
  // ParseContent(): the constructor reads the boxes, /Rotate and /Group, and
  // nothing reads the content stream or loads a font.
  RetainPtr<const CPDF_Dictionary> const_dict =
      pdf->GetPageDictionary(page_index);
  RetainPtr<CPDF_Dictionary> dict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
  if (!CPDF_Document::IsPageTreeLeaf(dict.Get())) {
    return 0;
  }
  auto page = pdfium::MakeRetain<CPDF_Page>(pdf, dict);
  const CFX_FloatRect& box = page->GetBBox();
  out[0] = box.left;
  out[1] = box.bottom;
  out[2] = box.right;
  out[3] = box.top;
  out[4] = static_cast<float>(page->GetPageRotation() * 90);
  out[5] = UserUnitOf(dict.Get());
  return 1;
}

}  // extern "C"
