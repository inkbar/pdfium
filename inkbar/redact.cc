// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See redact.h. A thin layer over EPDFPage_ApplyRedactions, which does the
// removing.

#include "inkbar/redact.h"

#include <limits.h>
#include <stdint.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "core/fpdfapi/edit/cpdf_text_redactor.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_appearancegenerator.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
#include "inkbar/appearance.h"
#include "inkbar/inspect.h"
#include "public/epdf_redact.h"
#include "public/fpdf_edit.h"

namespace {

constexpr char kDataKey[] = "INKB_Gloss";
constexpr char kOverlayKey[] = "INKB_Overlay";
// Quads of 8 floats a buffer may hold: 2^29 of them would wrap a 32-bit size.
constexpr int kMaxQuads = 1 << 29;

bool IsRedact(const CPDF_Dictionary* dict) {
  return dict && dict->GetNameFor("Subtype") == "Redact";
}

unsigned CountRedacts(const CPDF_Page* page) {
  RetainPtr<const CPDF_Array> annots = page->GetAnnotsArray();
  unsigned count = 0;
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    if (IsRedact(annots->GetDictAt(i).Get())) {
      ++count;
    }
  }
  return count;
}

// Closes the handles it holds.
class Handles {
 public:
  ~Handles() {
    for (FPDF_ANNOTATION annot : owned_) {
      FPDFPage_CloseAnnot(annot);
    }
  }
  void Own(FPDF_ANNOTATION annot) { owned_.push_back(annot); }

 private:
  std::vector<FPDF_ANNOTATION> owned_;
};

// The quads of a mark as the apply reads them: its /QuadPoints, each quad
// valid, else one quad from its /Rect. In /QuadPoints corner order.
std::vector<std::array<float, 8>> MarkQuads(const CPDF_Dictionary* dict) {
  std::vector<std::array<float, 8>> quads;
  RetainPtr<const CPDF_Array> points = dict->GetArrayFor("QuadPoints");
  if (points && points->size() >= 8) {
    const size_t count = CPDF_Annot::QuadPointCount(points.Get());
    for (size_t i = 0; i < count; ++i) {
      std::array<float, 8> quad;
      std::array<CFX_PointF, 4> corners;
      for (size_t k = 0; k < 8; ++k) {
        quad[k] = points->GetFloatAt(i * 8 + k);
      }
      for (size_t c = 0; c < 4; ++c) {
        corners[c] = CFX_PointF(quad[c * 2], quad[c * 2 + 1]);
      }
      std::optional<RedactRegion> region = RedactRegionFromQuadCorners(corners);
      if (!region.has_value() || region->bbox.IsEmpty()) {
        quads.clear();
        break;
      }
      quads.push_back(quad);
    }
    if (!quads.empty()) {
      return quads;
    }
  }
  CFX_FloatRect rect = dict->GetRectFor("Rect");
  rect.Normalize();
  if (!rect.IsEmpty()) {
    quads.push_back({rect.left, rect.top, rect.right, rect.top, rect.left,
                     rect.bottom, rect.right, rect.bottom});
  }
  return quads;
}

// Gives an own mark the tagged indirect /RO the apply flattens, so the check
// tells its overlay from content. A mark without /RO gets the overlay
// upstream would synthesise from its /IC and overlay text
// (CPDF_AppearanceGenerator::BuildRedactOverlayForm, as epdf_redact.cpp
// uses). One with an untagged /RO gets a tagged copy of it: the file may use
// that stream elsewhere too, as another annotation's appearance or a form the
// page draws, and a tag on it would hide those uses from the check. A mark
// for which no overlay can be built is left without one.
void TagOverlay(CPDF_Document* doc, CPDF_AnnotContext* context) {
  RetainPtr<CPDF_Dictionary> dict = context->GetMutableAnnotDict();
  if (!dict) {
    return;
  }
  RetainPtr<CPDF_Stream> overlay;
  if (RetainPtr<const CPDF_Stream> own = dict->GetStreamFor("RO")) {
    if (own->GetObjNum() && own->GetDict()->GetBooleanFor(kOverlayKey, false)) {
      return;
    }
    overlay = ToStream(own->CloneForHolder(doc));
  } else {
    overlay =
        CPDF_AppearanceGenerator::BuildRedactOverlayForm(doc, dict.Get());
  }
  if (!overlay) {
    return;
  }
  overlay->GetMutableDict()->SetNewFor<CPDF_Boolean>(kOverlayKey, true);
  // Indirect, so the flattened page draws this very stream and keeps the tag.
  const uint32_t objnum = overlay->GetObjNum()
                              ? overlay->GetObjNum()
                              : doc->AddIndirectObject(std::move(overlay));
  dict->SetNewFor<CPDF_Reference>("RO", doc, objnum);
}

}  // namespace

extern "C" {

int INK_Redact_SetOverlay(FPDF_DOCUMENT doc,
                          FPDF_ANNOTATION annot,
                          void* appearance) {
  auto* a = static_cast<INK_APPEARANCE*>(appearance);
  // Freed on every path; once installed, the document holds the stream.
  std::unique_ptr<INK_APPEARANCE, void (*)(INK_APPEARANCE*)> owned(
      a, INK_Appearance_Free);
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pdf || !a || !context || context->GetPage()->GetDocument() != pdf ||
      inkbar::AppearanceDocument(a) != pdf) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> dict = context->GetMutableAnnotDict();
  RetainPtr<CPDF_Stream> stream = inkbar::AppearanceStream(a);
  if (!IsRedact(dict.Get()) || !stream) {
    return 0;
  }
  stream->GetMutableDict()->SetNewFor<CPDF_Boolean>(kOverlayKey, true);
  // Indirect, so the flattened page draws this very stream and keeps the tag.
  const uint32_t objnum = pdf->AddIndirectObject(std::move(stream));
  dict->SetNewFor<CPDF_Reference>("RO", pdf, objnum);
  return 1;
}

int INK_Redact_ApplyPage(FPDF_DOCUMENT doc,
                         FPDF_PAGE page,
                         const FPDF_ANNOTATION* marks,
                         int count,
                         int own_locked,
                         unsigned* out_removed,
                         unsigned* out_left) {
  if (out_removed) {
    *out_removed = 0;
  }
  if (out_left) {
    *out_left = 0;
  }
  ScopedFPDFDocumentView view(doc);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!view.Get() || !pdf_page || pdf_page->GetDocument() != view.Get() ||
      count < 0 || (count > 0 && !marks)) {
    return -1;
  }

  std::vector<FPDF_ANNOTATION> apply;
  if (count > 0) {
    // SAFETY: the caller passes |count| handles at |marks|.
    for (FPDF_ANNOTATION mark :
         UNSAFE_BUFFERS(pdfium::span(marks, static_cast<size_t>(count)))) {
      CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(mark);
      if (!context || !epdf::IsAnnotationOfPage(pdf_page, context) ||
          !IsRedact(context->GetAnnotDict())) {
        return -1;
      }
      apply.push_back(mark);
    }
  }
  Handles handles;
  std::vector<FPDF_ANNOTATION> own;
  if (own_locked) {
    const int annots = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < annots; ++i) {
      FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
      if (!annot) {
        continue;
      }
      handles.Own(annot);
      const CPDF_Dictionary* dict =
          CPDFAnnotContextFromFPDFAnnotation(annot)->GetAnnotDict();
      if (IsRedact(dict) && dict->KeyExist(kDataKey)) {
        // Applied once even when also among |marks|: the engine dedupes.
        own.push_back(annot);
        apply.push_back(annot);
      }
    }
  }
  if (apply.empty()) {
    if (out_left) {
      *out_left = CountRedacts(pdf_page);
    }
    return 0;
  }

  // The apply cuts or clips an object that paints with a pattern, a shading
  // that can hold a picture or a soft mask's group, and leaves what it
  // carries in the file. Nothing is changed when one lies under a mark, or
  // when the page could not be read to tell.
  // A page not yet rendered or inspected is parsed first, as upstream's apply
  // does: an unparsed page would read as empty.
  pdf_page->ParseContent();
  std::vector<std::array<float, 8>> quads;
  for (FPDF_ANNOTATION mark : apply) {
    for (const std::array<float, 8>& quad :
         MarkQuads(CPDFAnnotContextFromFPDFAnnotation(mark)->GetAnnotDict())) {
      quads.push_back(quad);
    }
  }
  if (inkbar::UnremovableUnderQuads(pdf_page, quads) != 0) {
    if (out_left) {
      *out_left = CountRedacts(pdf_page);
    }
    return -1;
  }
  for (FPDF_ANNOTATION annot : own) {
    TagOverlay(view.Get(), CPDFAnnotContextFromFPDFAnnotation(annot));
  }

  uint32_t removed = 0;
  const bool applied = EPDFPage_ApplyRedactions(
      page, apply.data(), static_cast<int>(apply.size()), &removed);
  // The page changed, or may have on failure: a later page copy must not be
  // the one cached from before.
  inkbar::ForgetPageCopy(view.Get(), pdf_page->GetDict()->GetObjNum());
  if (!applied) {
    return -1;
  }
  if (!FPDFPage_GenerateContent(page)) {
    return -1;
  }
  if (out_removed) {
    *out_removed = removed;
  }
  if (out_left) {
    *out_left = CountRedacts(pdf_page);
  }
  return 1;
}

int INK_Redact_OwnLockedQuads(FPDF_DOCUMENT doc,
                              FPDF_PAGE page,
                              float* out,
                              int capacity) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!view.Get() || !pdf_page || pdf_page->GetDocument() != view.Get() ||
      capacity < 0 || capacity >= kMaxQuads || (capacity > 0 && !out)) {
    return -1;
  }
  // SAFETY: the caller passes room for |capacity| quads of 8 floats at |out|.
  pdfium::span<float> buffer =
      capacity ? UNSAFE_BUFFERS(
                     pdfium::span(out, static_cast<size_t>(capacity) * 8))
               : pdfium::span<float>();
  RetainPtr<const CPDF_Array> annots = pdf_page->GetAnnotsArray();
  size_t total = 0;
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> dict = annots->GetDictAt(i);
    if (!IsRedact(dict.Get()) || !dict->KeyExist(kDataKey)) {
      continue;
    }
    for (const std::array<float, 8>& quad : MarkQuads(dict.Get())) {
      if (total < static_cast<size_t>(capacity)) {
        for (size_t k = 0; k < 8; ++k) {
          buffer[total * 8 + k] = quad[k];
        }
      }
      ++total;
    }
  }
  return total > INT_MAX ? -1 : static_cast<int>(total);
}

}  // extern "C"
