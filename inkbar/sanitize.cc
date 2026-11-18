// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See sanitize.h. Visits every indirect object of the document and the direct
// objects inside it, so an entry is found wherever it sits, whether or not the
// page tree reaches it. Upstream's redaction removes the same copies from the
// pages and forms it redacts (cpdf_redaction_copies); this does it for the
// whole document.

#include "inkbar/sanitize.h"

#include <stdint.h>

#include <set>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// PDFium's parser nests direct objects at most 64 deep
// (kParserMaxRecursionDepth), so the walk never stops short of an object it
// parsed. An action chain longer than this is treated as a script.
constexpr int kMaxDepth = 256;

class Sanitizer {
 public:
  Sanitizer(CPDF_Document* doc, bool remove) : doc_(doc), remove_(remove) {}

  void Run() {
    Catalog();
    const uint32_t last = doc_->GetLastObjNum();
    for (uint32_t objnum = 1; objnum <= last; ++objnum) {
      // On a document that is not a layer the mutable object is the one the
      // document holds; a scan that finds without removing changes nothing.
      RetainPtr<CPDF_Object> object = doc_->GetMutableIndirectObject(objnum);
      if (object) {
        Visit(object.Get(), 0);
      }
    }
  }

  unsigned found() const { return found_; }
  unsigned attachments() const { return attachments_; }

 private:
  void Catalog() {
    RetainPtr<CPDF_Dictionary> root = doc_->GetMutableRoot();
    if (!root) {
      return;
    }
    if (RetainPtr<CPDF_Dictionary> names = root->GetMutableDictFor("Names")) {
      if (names->KeyExist("EmbeddedFiles")) {
        found_ |= INK_SANITIZE_ATTACHMENTS;
        std::unique_ptr<CPDF_NameTree> tree =
            CPDF_NameTree::CreateForReading(doc_, "EmbeddedFiles");
        attachments_ += tree ? static_cast<unsigned>(tree->GetCount()) : 0;
        Drop(names.Get(), "EmbeddedFiles");
      }
      if (names->KeyExist("JavaScript")) {
        found_ |= INK_SANITIZE_SCRIPTS;
        Drop(names.Get(), "JavaScript");
      }
    }
    if (root->KeyExist("Collection")) {
      found_ |= INK_SANITIZE_ATTACHMENTS;
      Drop(root.Get(), "Collection");
    }
    if (doc_->GetInfo()) {
      found_ |= INK_SANITIZE_METADATA;
      if (remove_) {
        doc_->DropInfo();
      }
    }
  }

  void Drop(CPDF_Dictionary* dict, const char* key) {
    if (remove_) {
      dict->RemoveFor(key);
    }
  }

  void DropIfPresent(CPDF_Dictionary* dict, const char* key, unsigned flag) {
    if (dict->KeyExist(key)) {
      found_ |= flag;
      Drop(dict, key);
    }
  }

  static bool HasScript(const CPDF_Object* object, int depth) {
    if (!object) {
      return false;
    }
    if (depth > kMaxDepth) {
      return true;
    }
    if (const CPDF_Array* array = object->AsArray()) {
      for (size_t i = 0; i < array->size(); ++i) {
        if (HasScript(array->GetDirectObjectAt(i).Get(), depth + 1)) {
          return true;
        }
      }
      return false;
    }
    const CPDF_Dictionary* action = object->AsDictionary();
    if (!action) {
      return false;
    }
    if (action->GetNameFor("S") == "JavaScript" || action->KeyExist("JS")) {
      return true;
    }
    return HasScript(action->GetDirectObjectFor("Next").Get(), depth + 1);
  }

  void Actions(CPDF_Dictionary* dict) {
    for (const char* key : {"A", "OpenAction"}) {
      if (HasScript(dict->GetDirectObjectFor(key).Get(), 0)) {
        found_ |= INK_SANITIZE_SCRIPTS;
        Drop(dict, key);
      }
    }
    RetainPtr<CPDF_Dictionary> triggers = dict->GetMutableDictFor("AA");
    if (!triggers) {
      return;
    }
    for (const ByteString& key : triggers->GetKeys()) {
      if (HasScript(triggers->GetDirectObjectFor(key.AsStringView()).Get(),
                    0)) {
        found_ |= INK_SANITIZE_SCRIPTS;
        Drop(triggers.Get(), key.c_str());
      }
    }
    if (remove_ && triggers->size() == 0) {
      dict->RemoveFor("AA");
    }
  }

  // File attachment annotations in a page's /Annots, and the pop-ups that
  // belong to them.
  void Annotations(CPDF_Dictionary* page) {
    RetainPtr<CPDF_Array> annots = page->GetMutableArrayFor("Annots");
    if (!annots) {
      return;
    }
    std::set<const CPDF_Dictionary*> removed;
    for (size_t i = annots->size(); i > 0; --i) {
      RetainPtr<CPDF_Dictionary> annot = annots->GetMutableDictAt(i - 1);
      if (!annot || annot->GetNameFor("Subtype") != "FileAttachment") {
        continue;
      }
      found_ |= INK_SANITIZE_ATTACHMENTS;
      ++attachments_;
      if (remove_) {
        // Also off the annotation itself, in case something else still
        // refers to it.
        annot->RemoveFor("FS");
        removed.insert(annot.Get());
        annots->RemoveAt(i - 1);
      }
    }
    for (size_t i = annots->size(); i > 0 && !removed.empty(); --i) {
      RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i - 1);
      if (annot && annot->GetNameFor("Subtype") == "Popup" &&
          removed.count(annot->GetDictFor("Parent").Get())) {
        annots->RemoveAt(i - 1);
      }
    }
  }

  void VisitDict(CPDF_Dictionary* dict, int depth) {
    DropIfPresent(dict, "Metadata", INK_SANITIZE_METADATA);
    DropIfPresent(dict, "Thumb", INK_SANITIZE_THUMBNAILS);
    DropIfPresent(dict, "PieceInfo", INK_SANITIZE_PRIVATE_DATA);
    DropIfPresent(dict, "OPI", INK_SANITIZE_PRIVATE_DATA);
    DropIfPresent(dict, "AF", INK_SANITIZE_ATTACHMENTS);
    Actions(dict);
    Annotations(dict);
    for (const ByteString& key : dict->GetKeys()) {
      RetainPtr<CPDF_Object> value =
          dict->GetMutableObjectFor(key.AsStringView());
      if (value && !value->IsReference()) {
        Visit(value.Get(), depth + 1);
      }
    }
  }

  void Visit(CPDF_Object* object, int depth) {
    if (depth > kMaxDepth) {
      return;
    }
    if (CPDF_Stream* stream = object->AsMutableStream()) {
      VisitDict(stream->GetMutableDict().Get(), depth);
    } else if (CPDF_Dictionary* dict = object->AsMutableDictionary()) {
      VisitDict(dict, depth);
    } else if (CPDF_Array* array = object->AsMutableArray()) {
      for (size_t i = 0; i < array->size(); ++i) {
        RetainPtr<CPDF_Object> item = array->GetMutableObjectAt(i);
        if (item && !item->IsReference()) {
          Visit(item.Get(), depth + 1);
        }
      }
    }
  }

  CPDF_Document* const doc_;
  const bool remove_;
  unsigned found_ = 0;
  unsigned attachments_ = 0;
};

}  // namespace

namespace inkbar {

unsigned FindSanitizable(CPDF_Document* doc, unsigned* out_attachment_count) {
  Sanitizer scan(doc, /*remove=*/false);
  scan.Run();
  if (out_attachment_count) {
    *out_attachment_count = scan.attachments();
  }
  return scan.found();
}

}  // namespace inkbar

extern "C" {

int INK_Sanitize(FPDF_DOCUMENT doc,
                 unsigned* out_removed,
                 unsigned* out_attachment_count) {
  if (out_removed) {
    *out_removed = 0;
  }
  if (out_attachment_count) {
    *out_attachment_count = 0;
  }
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf) {
    return 0;
  }
  Sanitizer sanitizer(pdf, /*remove=*/true);
  sanitizer.Run();
  if (out_removed) {
    *out_removed = sanitizer.found();
  }
  if (out_attachment_count) {
    *out_attachment_count = sanitizer.attachments();
  }
  return 1;
}

}  // extern "C"
