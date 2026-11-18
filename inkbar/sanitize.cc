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

#include <ctype.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
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
  explicit Sanitizer(CPDF_Document* doc) : doc_(doc) {}

  // Finds what there is, and with |remove| then removes it. The finding pass
  // runs first so that each embedded file is counted once, wherever it is
  // referred from, before any reference to it goes.
  void Run(bool remove) {
    Pass();
    if (remove) {
      remove_ = true;
      Pass();
    }
  }

  unsigned found() const { return found_; }
  unsigned attachments() const {
    return static_cast<unsigned>(embedded_.size() + unembedded_.size()) +
           unnamed_;
  }

 private:
  void Pass() {
    Catalog();
    const uint32_t last = doc_->GetLastObjNum();
    for (uint32_t objnum = 1; objnum <= last; ++objnum) {
      // On a document that is not a layer the mutable object is the one the
      // document holds; the finding pass changes nothing.
      RetainPtr<CPDF_Object> object = doc_->GetMutableIndirectObject(objnum);
      if (object) {
        Visit(object.Get(), 0);
      }
    }
  }

  void Catalog() {
    RetainPtr<CPDF_Dictionary> root = doc_->GetMutableRoot();
    if (!root) {
      return;
    }
    if (RetainPtr<CPDF_Dictionary> names = root->GetMutableDictFor("Names")) {
      // Embedded files are counted where their file specifications are
      // visited; an entry that names a file without embedding it counts
      // here, once per file specification, as an annotation's does.
      if (names->KeyExist("EmbeddedFiles")) {
        found_ |= INK_SANITIZE_ATTACHMENTS;
        if (!remove_) {
          std::unique_ptr<CPDF_NameTree> tree =
              CPDF_NameTree::CreateForReading(doc_, "EmbeddedFiles");
          for (size_t i = 0; tree && i < tree->GetCount(); ++i) {
            WideString name;
            RetainPtr<const CPDF_Dictionary> spec =
                ToDictionary(tree->LookupValueAndName(i, &name));
            if (spec && EmbeddedStreams(spec.Get()).empty()) {
              unembedded_.insert(spec.Get());
            }
          }
        }
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
    if (RetainPtr<CPDF_Dictionary> form = root->GetMutableDictFor("AcroForm")) {
      // An XFA form carries its own scripts.
      DropIfPresent(form.Get(), "XFA", INK_SANITIZE_SCRIPTS);
    }
    // The trailer's /Info may be a direct dictionary, which has no object
    // number and which GetInfo() does not read.
    const CPDF_Parser* parser = doc_->GetParser();
    const CPDF_Dictionary* trailer = parser ? parser->GetTrailer() : nullptr;
    // After DropInfo() GetInfo() is an empty direct dictionary, which is
    // nothing to remove.
    RetainPtr<const CPDF_Dictionary> info = doc_->GetInfo();
    const bool has_info = info && (info->GetObjNum() || info->size() > 0);
    if (has_info || (trailer && trailer->KeyExist("Info"))) {
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
    if (action->GetNameFor("S") == "URI" &&
        IsScriptUri(action->GetByteStringFor("URI"))) {
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
      RetainPtr<const CPDF_Dictionary> spec = annot->GetDictFor("FS");
      if (!remove_) {
        // A file named but not embedded is still an attachment, counted
        // once per file specification; one without any, once.
        if (!spec) {
          ++unnamed_;
        } else if (EmbeddedStreams(spec.Get()).empty()) {
          unembedded_.insert(spec.Get());
        }
      }
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
    // Any file specification's embedded files: attachments, and the targets
    // of GoToE, Launch, RichMedia, Screen and Sound alike.
    if (dict->KeyExist("EF")) {
      found_ |= INK_SANITIZE_ATTACHMENTS;
      for (const CPDF_Object* stream : EmbeddedStreams(dict)) {
        embedded_.insert(stream);
      }
      Drop(dict, "EF");
    }
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

  // The embedded file streams a file specification's /EF names.
  static std::vector<const CPDF_Object*> EmbeddedStreams(
      const CPDF_Dictionary* spec) {
    std::vector<const CPDF_Object*> streams;
    RetainPtr<const CPDF_Dictionary> files = spec->GetDictFor("EF");
    if (!files) {
      return streams;
    }
    CPDF_DictionaryLocker locker(files);
    for (const auto& [key, value] : locker) {
      RetainPtr<const CPDF_Object> stream = value->GetDirect();
      if (stream && stream->IsStream()) {
        streams.push_back(stream.Get());
      }
    }
    return streams;
  }

  // A URI that runs script when followed, read as a browser reads it: a
  // UTF-16BE or UTF-16LE string (with its byte order mark) decoded, leading control
  // characters and spaces stripped, tabs and line breaks dropped wherever
  // they are, and the scheme compared without regard to case.
  static bool IsScriptUri(const ByteString& uri) {
    pdfium::span<const uint8_t> bytes = uri.unsigned_span();
    std::vector<uint32_t> units;
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
      for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
        units.push_back((bytes[i] << 8) | bytes[i + 1]);
      }
    } else if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
      // UTF-16LE, which PDFium's text decoding also reads.
      for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
        units.push_back(bytes[i] | (bytes[i + 1] << 8));
      }
    } else {
      size_t from = 0;
      if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
          bytes[2] == 0xBF) {
        from = 3;
      }
      for (size_t i = from; i < bytes.size(); ++i) {
        units.push_back(bytes[i]);
      }
    }
    std::string scheme;
    bool leading = true;
    for (uint32_t unit : units) {
      if (leading && unit <= 0x20) {
        continue;
      }
      leading = false;
      if (unit == '\t' || unit == '\n' || unit == '\r') {
        continue;
      }
      scheme += unit < 0x80 ? static_cast<char>(tolower(unit)) : '\x80';
      if (scheme.size() == 11) {
        break;
      }
    }
    return scheme == "javascript:";
  }

  CPDF_Document* const doc_;
  bool remove_ = false;
  unsigned found_ = 0;
  // The embedded files found, each once. The document holds them for the
  // whole run, so their addresses name them.
  std::set<const CPDF_Object*> embedded_;
  // File specifications that name a file without embedding it.
  std::set<const CPDF_Dictionary*> unembedded_;
  // File attachment annotations with no file specification.
  unsigned unnamed_ = 0;
};

}  // namespace

namespace inkbar {

unsigned FindSanitizable(CPDF_Document* doc, unsigned* out_attachment_count) {
  Sanitizer scan(doc);
  scan.Run(/*remove=*/false);
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
  Sanitizer sanitizer(pdf);
  sanitizer.Run(/*remove=*/true);
  if (out_removed) {
    *out_removed = sanitizer.found();
  }
  if (out_attachment_count) {
    *out_attachment_count = sanitizer.attachments();
  }
  return 1;
}

}  // extern "C"
