// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Reports what lies under regions of a page, and what a file carries besides
// its pages, for the redaction check. Reports hold numbers and booleans only,
// never text from the file.

#ifndef INKBAR_INSPECT_H_
#define INKBAR_INSPECT_H_

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// A UTF-8 JSON report in a malloc'd buffer the caller frees with
// INK_Inspect_Free, or NULL when it could not inspect: no such page, a NULL
// quad pointer with a nonzero count, a quad that is not finite, a page whose
// box is not finite, a form nested past PDFium's parse limit of 40, a region
// PDFium's path redactor cannot use, or a page whose sampling tests need
// more work than one call allows (see the README). NULL never stands for
// "nothing found"; treat it as a failed check.
//
// |quads|: |count| quads of 8 floats each (x1,y1 ... x4,y4: upper left, upper
// right, lower left, lower right, as /QuadPoints), in page user space. One
// region per quad, kind "region". With |count| 0 the report lists candidates
// instead: every REDACT annotation (its /QuadPoints, else its /Rect, kind
// "redact-annotation"), filled boxes in the page content (kind "filled-box")
// and Square, Highlight and Stamp annotations drawn as one opaque filled
// rectangle (kind "annotation-box"), each with the counts under it.
//
// { "regions": [ { "index": 0, "rect": [l,b,r,t], "kind": "region",
//     "textChars": 0, "imagesNotBlank": 0, "paths": 0, "patternContent": 0,
//     "annotationText": 0, "annotationImages": 0 } ] }
char* INK_Inspect_Page(FPDF_DOCUMENT doc,
                       int page_index,
                       const float* quads,
                       int count);

// What the file carries besides its pages. |bytes| are the file's bytes, for
// counting its revisions; the rest is read from |doc|.
//
// { "revisions": 1, "info": false, "xmp": false, "attachments": 0,
//   "scripts": false, "hiddenLayers": false, "signed": false }
char* INK_Inspect_File(FPDF_DOCUMENT doc,
                       const unsigned char* bytes,
                       unsigned long size);

// Frees a report. NULL is a no-op.
void INK_Inspect_Free(char* report);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_INSPECT_H_
