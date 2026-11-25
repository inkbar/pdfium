// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Reports what a file says about a page's annotations and the order its
// structure tree gives them, and draws one annotation's appearance. Each
// report says what the file holds and decides nothing: which annotations a
// viewer lists, what a flag means or what a colour of five components is, is
// the caller's to decide.

#ifndef INKBAR_ANNOTS_H_
#define INKBAR_ANNOTS_H_

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Each report is UTF-8 JSON in a malloc'd buffer freed with INK_Json_Free, or
// NULL when it could not be read (no document, no such page). Reports write at
// most 16 MB: an entry that would pass that is left out, the report stops
// there, and its "truncated" is true.
//
// Text = "the text" | { "bytes": "<hex>" }
// A text string of the file as text, or as its bytes in hex when they are
// neither UTF-16 with a byte order mark (an odd length or an unpaired surrogate
// is not), UTF-8 with one, nor PDFDocEncoding (0x7F, 0x9F and 0xAD are
// undefined there). Text is cut after the last character whose written form
// (UTF-8 with JSON escapes) ends within 1 MB, hex after 512 KB of bytes; an
// entry with a cut value has "truncated": true. A name is written as its
// bytes, each the code point of its value (U+0000 to U+00FF). A number is
// written with 9 significant digits; one that is absent where a number is
// due, not a number or not finite is null. A reference is [number,
// generation], the generation that of the object the file holds under the
// number, 0 when none.

// Reads /Annots from the page dictionary without parsing the page's content,
// so it loads no font.
// { "annots": [ {
//     "index": 0, "subtype": "Text", "objectNumber": 12, "generation": 0,
//     "rect": [l, b, r, t] | null, "rectOk": true, "flags": 4,
//     "hasAppearance": true, "appearance": "stream" | "states" | null,
//     "states": ["Off", "Yes"], "as": "Off",
//     "quads": [...], "color": [...], "interiorColor": [...], "opacity": 0.5,
//     "contents": Text, "author": Text, "created": Text, "modified": Text,
//     "icon": "Comment", "inReplyTo": [7, 0], "intent": "FreeTextCallout",
//     "line": [x1, y1, x2, y2], "lineEndings": ["None", "OpenArrow"],
//     "vertices": [...], "inkLists": [[...], ...],
//     "borderStyle": { "width": 1, "style": "S" }, "border": [0, 0, 1],
//     "da": Text, "q": 1, "pmd": true, "parent": [9, 0],
//     "truncated": true } ],
//   "tabs": "S", "truncated": false }
// "annots" has an entry for every dictionary in /Annots, in order, "index" its
// place there; any other entry, a stream included, is skipped and the entries
// after it keep their places. At most 200,000 entries. "objectNumber" and
// "generation" are absent for a direct dictionary. "rect", "rectOk",
// "hasAppearance", "appearance", "states", "as", "borderStyle" and "border"
// are described in report.h. "flags" is /F. The other keys are present only
// when the file has the entry in the right type: "quads" /QuadPoints,
// "color" /C, "interiorColor" /IC, "line" /L and "vertices" /Vertices are
// arrays of numbers as written, any length; "inkLists" is /InkList, an array
// of arrays (an entry that is not an array is null). /QuadPoints, /Vertices
// and /InkList as a whole are read up to 1,000,000 numbers, and an entry cut
// there has "truncated": true. "opacity" is /CA; "contents" /Contents,
// "author" /T, "created" /CreationDate and "modified" /M are text strings.
// "icon" is /Name, "intent" /IT and "lineEndings" /LE names (an entry of /LE
// that is not a name is null). "inReplyTo" is /IRT when it is a reference;
// it may name the annotation itself. "da" is /DA and "q" /Q, the
// annotation's own, not inherited. "pmd" is true when /PMD is present.
// "parent" is /Parent when it is a reference. "tabs" is the page's /Tabs, a
// name, or null.
char* INK_Annots_Page(FPDF_DOCUMENT doc, int page_index);

// { "order": [12, 15, 13], "truncated": false }
// The object numbers of page |page_index|'s annotations in the order the
// structure tree (/StructTreeRoot /K) reaches them: depth first through /K,
// each object reference (/Type /OBJR) whose /Obj is a reference to an
// annotation in the page's /Annots, each annotation once. Annotations the tree
// does not reach are not listed. The walk visits each object once, goes at
// most 1,000 levels deep and reads at most 1,000,000 objects; "truncated" is
// true when either cap cut it short. A file with no structure tree gives
// "order": [].
char* INK_Struct_Order(FPDF_DOCUMENT doc, int page_index);

// Draws the normal appearance of the annotation at place |annot_index| of the
// page's /Annots into |bitmap|, as EPDF_RenderAnnotBitmap draws it: |matrix|
// (a, b, c, d, e, f; NULL for none) is applied after the page's display
// matrix, so [sx 0 0 sy 0 0] puts the page's top left at the bitmap's origin,
// sx being the bitmap's width over the page's display width and sy likewise.
// |state| is a state of /AP /N to draw whatever /AS says, as its name's bytes
// with a NUL (the report's code points U+0000 to U+00FF are those bytes);
// NULL or "" draws the state /AS selects. A state /N lacks, or /N that is a
// stream with a state given, draws nothing. Nothing is written to the
// document: a widget with no appearance is drawn from memory or not at all.
// Returns 1 when it drew, 0 otherwise: a NULL bitmap or page, no dictionary
// at |annot_index|, an annotation with no rectangle to draw in, or a page an
// InkRender holds (INK_Render_Start to INK_Render_Close).
int INK_Annot_Draw(FPDF_BITMAP bitmap,
                   FPDF_PAGE page,
                   int annot_index,
                   const char* state,
                   const float* matrix);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_ANNOTS_H_
