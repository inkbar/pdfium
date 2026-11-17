// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See appearance.h. Builds the objects with PDFium's parser classes, and
// reuses the code behind EPDFImageObj_SetPng, EPDFImageObj_SetJpeg and
// FPDF_NewXObjectFromPage for images and pages.

#include "inkbar/appearance.h"

#include <cmath>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "core/fpdfapi/edit/cpdf_npagetooneexporter.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/ipdf_page.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/cfx_read_only_container_stream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/observed_ptr.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

struct ink_appearance_t {
  CPDF_Document* doc;
  CFX_FloatRect bbox;
  // Not yet an indirect object: freeing the appearance drops it.
  RetainPtr<CPDF_Stream> stream;
};

namespace {

constexpr char kDataKey[] = "INKB_Gloss";

// The font and page objects a document already holds for appearances, so each
// base font and page gets one. Forgotten when the document is destroyed.
class DocCache final : public Observable::ObserverIface {
 public:
  explicit DocCache(CPDF_Document* doc) : doc_(doc) { doc_->AddObserver(this); }
  ~DocCache() override = default;

  // Erases this cache, which deletes it.
  void OnObservableDestroyed() override;

  std::map<ByteString, uint32_t> fonts;  // base font -> font dictionary
  std::map<uint32_t, uint32_t> pages;    // page dictionary -> Form XObject

 private:
  CPDF_Document* const doc_;
};

std::map<CPDF_Document*, std::unique_ptr<DocCache>>& Caches() {
  static auto* caches = new std::map<CPDF_Document*, std::unique_ptr<DocCache>>;
  return *caches;
}

void DocCache::OnObservableDestroyed() {
  Caches().erase(doc_);
}

DocCache* CacheFor(CPDF_Document* doc) {
  auto& caches = Caches();
  auto it = caches.find(doc);
  if (it == caches.end()) {
    it = caches.emplace(doc, std::make_unique<DocCache>(doc)).first;
  }
  return it->second.get();
}

// [A-Za-z0-9]+, or with |dash| also '-'. Resource names and base fonts are
// written as PDF names; these characters need no escaping.
bool IsPlainName(const char* name, bool dash = false) {
  if (!name || !*name) {
    return false;
  }
  for (char c : std::string_view(name)) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || (dash && c == '-');
    if (!ok) {
      return false;
    }
  }
  return true;
}

pdfium::span<const uint8_t> Bytes(const unsigned char* data, size_t len) {
  if (!len) {
    return {};
  }
  // SAFETY: the caller passes |len| readable bytes at |data|.
  return UNSAFE_BUFFERS(pdfium::span<const uint8_t>(data, len));
}

// A new direct stream holding |data| Flate-compressed.
RetainPtr<CPDF_Stream> NewFlateStream(CPDF_Document* doc,
                                      pdfium::span<const uint8_t> data) {
  auto dict = doc->New<CPDF_Dictionary>();
  dict->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
  return pdfium::MakeRetain<CPDF_Stream>(FlateModule::Encode(data),
                                         std::move(dict));
}

RetainPtr<CPDF_Dictionary> ResourceDict(INK_APPEARANCE* a, const char* kind) {
  return a->stream->GetMutableDict()
      ->GetOrCreateDictFor("Resources")
      ->GetOrCreateDictFor(kind);
}

void SetResource(INK_APPEARANCE* a,
                 const char* kind,
                 const char* name,
                 uint32_t objnum) {
  ResourceDict(a, kind)->SetNewFor<CPDF_Reference>(name, a->doc, objnum);
}

uint32_t FontObject(CPDF_Document* doc, const ByteString& base_font) {
  DocCache* cache = CacheFor(doc);
  auto it = cache->fonts.find(base_font);
  if (it != cache->fonts.end()) {
    RetainPtr<const CPDF_Dictionary> held =
        ToDictionary(doc->GetIndirectObject(it->second));
    if (held && held->GetByteStringFor("BaseFont") == base_font) {
      return it->second;
    }
  }
  auto font = doc->NewIndirect<CPDF_Dictionary>();
  font->SetNewFor<CPDF_Name>("Type", "Font");
  font->SetNewFor<CPDF_Name>("Subtype", "Type1");
  font->SetNewFor<CPDF_Name>("BaseFont", base_font);
  font->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
  cache->fonts[base_font] = font->GetObjNum();
  return font->GetObjNum();
}

// The Form XObject of page |page_index|, made once per page. 0 on failure.
uint32_t PageObject(CPDF_Document* doc, int page_index) {
  RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(page_index);
  if (!page) {
    return 0;
  }
  DocCache* cache = CacheFor(doc);
  const uint32_t page_objnum = page->GetObjNum();
  if (page_objnum) {
    auto it = cache->pages.find(page_objnum);
    if (it != cache->pages.end() &&
        ToStream(doc->GetIndirectObject(it->second))) {
      return it->second;
    }
  }
  CPDF_NPageToOneExporter exporter(doc, doc);
  std::unique_ptr<XObjectContext> xobject =
      exporter.CreateXObjectContextFromPage(page_index);
  if (!xobject || !xobject->xobject) {
    return 0;
  }
  const uint32_t objnum = xobject->xobject->GetObjNum();
  if (page_objnum) {
    cache->pages[page_objnum] = objnum;
  }
  return objnum;
}

}  // namespace

extern "C" {

INK_APPEARANCE* INK_Appearance_New(FPDF_DOCUMENT doc,
                                   float left,
                                   float bottom,
                                   float right,
                                   float top,
                                   const unsigned char* content,
                                   size_t content_len,
                                   int transparency_group) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || (!content && content_len)) {
    return nullptr;
  }
  if (!std::isfinite(left) || !std::isfinite(bottom) ||
      !std::isfinite(right) || !std::isfinite(top) || !(left < right) ||
      !(bottom < top)) {
    return nullptr;
  }

  auto a = std::make_unique<INK_APPEARANCE>();
  a->doc = pdf;
  a->bbox = CFX_FloatRect(left, bottom, right, top);
  a->stream = NewFlateStream(pdf, Bytes(content, content_len));
  RetainPtr<CPDF_Dictionary> dict = a->stream->GetMutableDict();
  dict->SetNewFor<CPDF_Name>("Type", "XObject");
  dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  dict->SetRectFor("BBox", a->bbox);
  dict->SetMatrixFor("Matrix", CFX_Matrix());
  dict->SetNewFor<CPDF_Dictionary>("Resources");
  if (transparency_group) {
    dict->SetNewFor<CPDF_Dictionary>("Group")->SetNewFor<CPDF_Name>(
        "S", "Transparency");
  }
  return a.release();
}

int INK_Appearance_AddFont(INK_APPEARANCE* a,
                           const char* name,
                           const char* base_font) {
  if (!a || !IsPlainName(name) || !IsPlainName(base_font, /*dash=*/true)) {
    return 0;
  }
  ScopedFPDFDocumentView view(FPDFDocumentFromCPDFDocument(a->doc));
  SetResource(a, "Font", name, FontObject(a->doc, ByteString(base_font)));
  return 1;
}

int INK_Appearance_AddOpacity(INK_APPEARANCE* a,
                              const char* name,
                              float fill,
                              float stroke) {
  if (!a || !IsPlainName(name) || std::isnan(fill) || std::isnan(stroke) ||
      fill > 1 || stroke > 1) {
    return 0;
  }
  ScopedFPDFDocumentView view(FPDFDocumentFromCPDFDocument(a->doc));
  auto state = ResourceDict(a, "ExtGState")->SetNewFor<CPDF_Dictionary>(name);
  if (fill >= 0) {
    state->SetNewFor<CPDF_Number>("ca", fill);
  }
  if (stroke >= 0) {
    state->SetNewFor<CPDF_Number>("CA", stroke);
  }
  return 1;
}

int INK_Appearance_AddImage(INK_APPEARANCE* a,
                            const char* name,
                            int format,
                            const unsigned char* data,
                            size_t len) {
  if (!a || !IsPlainName(name) || !data || (format != 0 && format != 1)) {
    return 0;
  }
  // The same lower bounds EPDFImageObj_SetPng and EPDFImageObj_SetJpeg check.
  if (len < (format == 0 ? 8u : 2u)) {
    return 0;
  }
  ScopedFPDFDocumentView view(FPDFDocumentFromCPDFDocument(a->doc));
  auto image = pdfium::MakeRetain<CPDF_Image>(a->doc);
  pdfium::span<const uint8_t> bytes = Bytes(data, len);
  if (format == 0) {
    image->SetPng(bytes.data(), bytes.size());
  } else {
    DataVector<uint8_t> copy(bytes.begin(), bytes.end());
    image->SetJpegImageInline(
        pdfium::MakeRetain<CFX_ReadOnlyDataVectorStream>(std::move(copy)));
  }
  // Both leave the image without a stream when the bytes do not decode.
  if (!image->GetStream()) {
    return 0;
  }
  image->ConvertStreamToIndirectObject();
  const uint32_t objnum = image->GetStream()->GetObjNum();
  // Smoothed when enlarged, as Inkbar's screen draws it. The soft mask gets
  // the entry too (ISO 32000-1 Table 145 allows it), so its edge matches.
  RetainPtr<CPDF_Stream> stream =
      ToStream(a->doc->GetMutableIndirectObject(objnum));
  if (!stream) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> dict = stream->GetMutableDict();
  dict->SetNewFor<CPDF_Boolean>("Interpolate", true);
  RetainPtr<const CPDF_Stream> smask = dict->GetStreamFor("SMask");
  if (smask && smask->GetObjNum()) {
    RetainPtr<CPDF_Stream> mask =
        ToStream(a->doc->GetMutableIndirectObject(smask->GetObjNum()));
    if (mask) {
      mask->GetMutableDict()->SetNewFor<CPDF_Boolean>("Interpolate", true);
    }
  }
  SetResource(a, "XObject", name, objnum);
  return 1;
}

int INK_Appearance_AddPage(INK_APPEARANCE* a,
                           const char* name,
                           int page_index) {
  if (!a || !IsPlainName(name)) {
    return 0;
  }
  ScopedFPDFDocumentView view(FPDFDocumentFromCPDFDocument(a->doc));
  if (page_index < 0 || page_index >= a->doc->GetPageCount()) {
    return 0;
  }
  const uint32_t objnum = PageObject(a->doc, page_index);
  if (!objnum) {
    return 0;
  }
  SetResource(a, "XObject", name, objnum);
  return 1;
}

int INK_Appearance_AddForm(INK_APPEARANCE* a,
                           const char* name,
                           INK_APPEARANCE* child) {
  std::unique_ptr<INK_APPEARANCE> owned(child == a ? nullptr : child);
  if (!a || !IsPlainName(name) || !owned || owned->doc != a->doc) {
    return 0;
  }
  ScopedFPDFDocumentView view(FPDFDocumentFromCPDFDocument(a->doc));
  const uint32_t objnum = a->doc->AddIndirectObject(owned->stream);
  SetResource(a, "XObject", name, objnum);
  return 1;
}

void INK_Appearance_Free(INK_APPEARANCE* a) {
  delete a;
}

int INK_Annot_SetAppearance(FPDF_ANNOTATION annot, INK_APPEARANCE* a) {
  std::unique_ptr<INK_APPEARANCE> owned(a);
  ScopedFPDFAnnotationView view(annot);
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!owned || !context || context->GetPage()->GetDocument() != owned->doc) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> dict = context->GetMutableAnnotDict();
  if (!dict) {
    return 0;
  }
  const uint32_t objnum = owned->doc->AddIndirectObject(owned->stream);
  auto ap = dict->SetNewFor<CPDF_Dictionary>("AP");
  // A stream /N has no states, so an /AS left behind would name none.
  dict->RemoveFor("AS");
  ap->SetNewFor<CPDF_Reference>("N", owned->doc, objnum);
  dict->SetRectFor("Rect", owned->bbox);
  return 1;
}

int INK_Annot_SetData(FPDF_ANNOTATION annot,
                      const unsigned char* json,
                      size_t len) {
  ScopedFPDFAnnotationView view(annot);
  CPDF_AnnotContext* context = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!context || (!json && len)) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> dict = context->GetMutableAnnotDict();
  if (!dict) {
    return 0;
  }
  if (!len) {
    dict->RemoveFor(kDataKey);
    return 1;
  }
  CPDF_Document* doc = context->GetPage()->GetDocument();
  const uint32_t objnum =
      doc->AddIndirectObject(NewFlateStream(doc, Bytes(json, len)));
  dict->SetNewFor<CPDF_Reference>(kDataKey, doc, objnum);
  return 1;
}

}  // extern "C"
