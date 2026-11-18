// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Removes from a whole document what a redacted file must not carry besides
// its page content: metadata, attached files, scripts, thumbnails and private
// application data.

#ifndef INKBAR_SANITIZE_H_
#define INKBAR_SANITIZE_H_

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bit flags of what was present and removed, written to *out_removed.
//   METADATA     the trailer's /Info and every /Metadata stream (the
//                catalog's XMP, and XMP on pages, forms and images)
//   ATTACHMENTS  /Names /EmbeddedFiles, file attachment annotations, /AF
//                arrays and the catalog's /Collection
//   SCRIPTS      /Names /JavaScript, and every /A, /OpenAction and /AA entry
//                whose action, or an action in its /Next chain, is a
//                JavaScript action or carries /JS
//   THUMBNAILS   /Thumb
//   PRIVATE_DATA /PieceInfo, and /OPI links to full-resolution originals
enum {
  INK_SANITIZE_METADATA = 1,
  INK_SANITIZE_ATTACHMENTS = 2,
  INK_SANITIZE_SCRIPTS = 4,
  INK_SANITIZE_THUMBNAILS = 8,
  INK_SANITIZE_PRIVATE_DATA = 16
};

// Removes all of the above from every object of |doc|. Writes the flags of
// what it found to *out_removed and the number of attached files removed
// (entries of /EmbeddedFiles and file attachment annotations) to
// *out_attachment_count. Either pointer may be NULL. A full save afterwards
// writes none of it; an incremental save keeps the earlier revisions that
// hold it. Returns 1 on success, 0 for a NULL document.
int INK_Sanitize(FPDF_DOCUMENT doc,
                 unsigned* out_removed,
                 unsigned* out_attachment_count);

#ifdef __cplusplus
}  // extern "C"

class CPDF_Document;

namespace inkbar {
// What INK_Sanitize would remove from |doc|, as its flags, without removing
// it. Writes the attachment count as INK_Sanitize does.
unsigned FindSanitizable(CPDF_Document* doc, unsigned* out_attachment_count);
}  // namespace inkbar
#endif

#endif  // INKBAR_SANITIZE_H_
