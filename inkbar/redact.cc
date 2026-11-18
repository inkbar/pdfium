// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See redact.h. A thin layer over EPDFPage_ApplyRedactions, which does the
// removing.

#include "inkbar/redact.h"

#include <stdint.h>

#include <memory>
#include <vector>

#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_object_helpers.h"
#include "inkbar/appearance.h"
#include "public/epdf_redact.h"
#include "public/fpdf_edit.h"

namespace {

constexpr char kDataKey[] = "INKB_Gloss";
constexpr char kOverlayKey[] = "INKB_Overlay";

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

}  // extern "C"
