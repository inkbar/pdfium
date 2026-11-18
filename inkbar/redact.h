// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Applies redact annotations with PDFium's redaction engine
// (EPDFPage_ApplyRedactions), and gives a redact annotation the overlay it
// leaves behind.

#ifndef INKBAR_REDACT_H_
#define INKBAR_REDACT_H_

#include "public/fpdf_annot.h"
#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Gives a REDACT annotation an /RO overlay from an appearance built with
// INK_Appearance_* (the handle type INK_Annot_SetAppearance takes), and tags
// the overlay's form dictionary with /INKB_Overlay true. The apply flattens
// that same stream into the page, so the tag stays on the form the page draws
// and the redaction check can tell the overlay from content. Takes
// |appearance| whether or not it succeeds. Fails when the annotation is not a
// REDACT annotation of |doc|, or the appearance belongs to another document.
// Returns 1 on success.
int INK_Redact_SetOverlay(FPDF_DOCUMENT doc,
                          FPDF_ANNOTATION annot,
                          void* appearance);

// Applies REDACT annotations of |page|: those at |marks| (count > 0), then
// every other REDACT annotation of the page that carries /INKB_Gloss when
// |own_locked| is 1. Each such own mark first gets an overlay tagged
// /INKB_Overlay true: a tagged copy of its /RO in the /RO's place (the file
// may use the stream elsewhere, and the tag must not reach those uses), or,
// without one, the overlay upstream would synthesise from its /IC and overlay
// text, installed as its /RO. An /RO that already carries the tag is kept.
// Calls FPDFPage_GenerateContent. Writes the number of non-REDACT annotations
// removed to *out_removed and the number of REDACT annotations left on the
// page to *out_left. Returns 1 when applied, 0 when there was nothing to
// apply, -1 on failure. The caller closes the handles at |marks| afterwards.
// On failure the page may be partly changed: reload the document.
int INK_Redact_ApplyPage(FPDF_DOCUMENT doc,
                         FPDF_PAGE page,
                         const FPDF_ANNOTATION* marks,
                         int count,
                         int own_locked,
                         unsigned* out_removed,
                         unsigned* out_left);

// Writes the quads of the REDACT annotations of |page| that carry
// /INKB_Gloss, in /Annots order: 8 floats per quad in the corner order
// INK_Inspect_Page takes, a mark's /QuadPoints when it has them, else one
// quad from its /Rect (quads that are not finite or empty fall back to the
// /Rect, as the apply does). Writes at most |capacity| quads to |out| (which
// may be NULL when |capacity| is 0) and returns the total number of quads, so
// a caller can size a buffer and call again. Returns -1 on failure, and for
// a |capacity| of 2^29 quads or more. Call it
// before INK_Redact_ApplyPage, which removes the marks.
int INK_Redact_OwnLockedQuads(FPDF_DOCUMENT doc,
                              FPDF_PAGE page,
                              float* out,
                              int capacity);

// What is painted under each region of page |page_index| that the apply
// cannot rewrite, as a UTF-8 JSON report in a malloc'd buffer the caller
// frees with INK_Inspect_Free. Call it before INK_Redact_ApplyPage, on the
// page as it is: the apply cuts or clips these objects and leaves what they
// carry in the file, and afterwards the cut object no longer says what it
// covered. An apply over a region with any count above zero must be refused.
// |quads| as INK_Inspect_Page takes them; |count| > 0. NULL when it could
// not look (no such page, a bad quad, a limit met, the work budget spent).
//
// Counted are the objects, in the page content, in forms at any depth and
// in annotation appearances, that paint in the region (their shape and every
// clip in effect, those of enclosing form objects included) and are:
//   patterns: filled, stroked or shown with a tiling pattern;
//   shadings: a function-based or mesh shading (types 1, 4 to 7), painted
//     with sh or as a shading pattern;
//   maskGroups: under a soft mask whose group holds anything in the region;
//   glyphs: text in a Type 3 font with a glyph whose procedure paints in
//     the region though the box it declares does not meet it. A glyph whose
//     box meets the region is removed with its text and is not counted.
//
// { "regions": [ { "index": 0, "patterns": 0, "shadings": 0,
//     "maskGroups": 0, "glyphs": 0 } ] }
char* INK_Redact_Unremovable(FPDF_DOCUMENT doc,
                             int page_index,
                             const float* quads,
                             int count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_REDACT_H_
