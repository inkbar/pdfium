// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Reports what a file says about a page's annotations and links, its outline
// and its named destinations. Each call reports facts and decides nothing:
// byte strings from the file are hex ("...Hex"), unchanged, and a JavaScript
// action's source is text that nothing runs.

#ifndef INKBAR_LINKS_H_
#define INKBAR_LINKS_H_

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Each call returns UTF-8 JSON in a malloc'd buffer freed with INK_Json_Free,
// or NULL when it could not read (no such page, no document). Only the first
// action of a chain is read.
//
// Dest   = { "page": 3, "pageRef": [12, 0] | null, "fit": "XYZ",
//            "args": [72, 700, null, null] }
//        | { "nameHex": "636861707465722d32" }
// "page" is the index of the page the first entry names, or -1 when it names no
// page of this file: a page dictionary not in the page tree, a number that is
// negative or not less than the page count, anything else. A remote go-to's or
// a launch's "dest" names a page of another file, so there a number is given as
// the file has it. "pageRef" is the object number and generation the first
// entry refers to, or null when it is not a reference; the generation is that
// of the object the file holds under the number (PDFium keeps none in a
// reference), 0 when there is none. "fit" is one of XYZ, Fit, FitH, FitV, FitR,
// FitB, FitBH, FitBV, else "Unknown". "args" are the four entries after the fit
// name, each a number or null (absent, null or not a number). A string or name
// is a named destination, not looked up. Anything else is null.
//
// Action = { "s": "URI", "uriHex": "...", "uriKind": "string" | "name" | null }
//        | { "s": "GoTo", "dest": Dest | null }
//        | { "s": "GoToR" | "Launch", "fileHex": "..." | null,
//            "fileKind": "string" | "spec" | null, "dest": Dest | null,
//            "newWindow": true | false | null }
//        | { "s": "Named", "name": "NextPage" }
//        | { "s": "JavaScript", "js": "app.launchURL('...')",
//            "scriptTruncated": true }
//        | { "s": "<any other /S>" }
// "uriHex" is /URI's bytes, a string's or a name's; "uriKind" says which, and
// is null, with "uriHex" empty, when /URI is neither. "fileHex" is /F when it
// is a string, else the first string of a file specification's /UF, /F, /Unix,
// /Mac and /DOS; "fileKind" says whether /F was a string or a file
// specification, null when it was neither. "js" is /JS decoded as PDFium
// decodes it, from a string or a stream, and cut after the last character
// whose written form, as UTF-8 with JSON escapes (6 bytes for a control
// character, 2 for a quote or a backslash), ends within 4,096 bytes. On one
// page the scripts of all its links write at most 1,048,576 such bytes in
// all; once that is spent, each later script is written empty.
// "scriptTruncated" is present, and true, only when a script was cut. A
// name's bytes above 0x7F are written as the code points U+0080 to U+00FF.

// Reads /Annots from the page dictionary without parsing the page's content.
// { "annots": [ { "index": 0, "subtype": "Link", "flags": 4, "obj": 12,
//                 "generation": 0, "rectOk": true, "hasQuads": false,
//                 "quadsOk": true } ],
//   "links":  [ { "index": 0, "rect": [l,b,r,t], "quads": [x1,y1,...] | null,
//                 "dest": Dest | null, "destInvalid": true,
//                 "action": Action | null, "aa": Action | null } ] }
// "annots" has an entry for every dictionary in /Annots, "index" its place
// there; any other entry, a stream included, is skipped. "obj" is 0 for a
// direct object, and so is "generation". "rectOk": /Rect is 4 finite numbers.
// "hasQuads": /QuadPoints is present and not null, whatever it holds.
// "quadsOk": /QuadPoints is absent, or a non-empty array of finite numbers
// whose length is a multiple of 8. "links" has the Link annotations: "rect"
// normalised, [0,0,0,0] when /Rect is not well formed; "quads" null when
// /QuadPoints is absent or not well formed. "destInvalid" is present, and true,
// only when /Dest is in the dictionary (null included) but is not a string, a
// name or an array; "dest" is then null. It covers nothing else: an array that
// is no valid explicit destination (a bad page entry, a wrong number of
// arguments) is reported as "dest", and a caller applies the checks for an
// explicit destination to "dest" itself. "action" is null when /A is absent
// or not a dictionary. "aa" is /AA's /D, else its /U.
char* INK_Links_Page(FPDF_DOCUMENT doc, int page_index);

// { "items": [ { "depth": 0, "title": "Cover", "flags": 2, "count": -2,
//                "dest": Dest | null, "action": Action | null } ],
//   "truncated": false }
// Depth first. "flags" is /F (1 italic, 2 bold), "count" /Count. Stops at
// depth 64 (depths 0 to 63) and at 100,000 items, and never visits an object
// twice; "truncated" is true when any of the three cut it short.
char* INK_Outline(FPDF_DOCUMENT doc);

// |name|: |len| bytes as the file has them. Looks in /Names /Dests, then the
// catalog's /Dests, and returns an explicit Dest, or "null" when the name is
// unknown, and for a NULL |name| or a |len| of 0 or less.
char* INK_Dest_Named(FPDF_DOCUMENT doc, const unsigned char* name, int len);

// Frees JSON from the calls above. NULL is a no-op.
void INK_Json_Free(char* json);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_LINKS_H_
