# Inkbar PDFium

This is Inkbar's fork of PDFium. It is built for `@inkbar/engine`, the PDF engine behind Inkbar.

## Provenance

The history here comes from three sources, in order.

1. PDFium, from https://pdfium.googlesource.com/pdfium. A fresh clone has no remote for it. Add one with `git remote add upstream https://pdfium.googlesource.com/pdfium`.
2. `embedpdf/runtime`, from https://github.com/embedpdf/runtime. It is an Apache-2.0 fork of PDFium that adds editing and WebAssembly work.
3. Inkbar's own commits, which follow the pinned commit.

The pinned commit is `f2ea7597287f26bdcb450602de6616ff6939d506` on the `embedpdf/main` branch of `embedpdf/runtime`. Everything after it in this repository's history is Inkbar's.

## Layout

Everything Inkbar adds lives under `inkbar/`. Changes to files outside that directory are kept small and are recorded in the commit history.

| File | Contents |
| --- | --- |
| `shim.c` | What a JavaScript host needs and PDFium's C API leaves to the embedder: starting the library, saving to memory, form-fill setup. |
| `appearance.h`, `appearance.cc` | `INK_Appearance_*` and `INK_Annot_*`. They build a Form XObject from content the caller draws, name its fonts, opacities, images, pages and nested forms, and install it as an annotation's `/AP /N`. They also store Inkbar's annotation data as a Flate stream under `/INKB_Gloss`. |
| `redact.h`, `redact.cc` | `INK_Redact_*`. `INK_Redact_SetOverlay` gives a redact annotation its overlay, tagged `/INKB_Overlay true`. `INK_Redact_ApplyPage` applies a page's redact annotations through `EPDFPage_ApplyRedactions` and regenerates the page content. With `own_locked`, each of Inkbar's own marks first gets a tagged overlay, so the check tells the drawn fill from content. A mark whose `/RO` already carries the tag keeps it. A mark with an untagged `/RO` gets a tagged copy of that stream as its `/RO`: the file may use the stream elsewhere too, as another annotation's appearance or a form the page draws, and a tag on the shared stream would hide those uses from the check. A mark without an `/RO` gets upstream's synthesised overlay (from `/IC` and overlay text), tagged. `INK_Redact_Unremovable`, called before an apply, counts what is painted under each region that the apply cannot rewrite (see below). `INK_Redact_ApplyPage` asks the same of its own marks and returns -1, changing nothing, when anything counts. `INK_Redact_OwnLockedQuads`, called before an apply, lists the quads of the page's redact annotations that carry `/INKB_Gloss`, as the apply reads them, so the caller can check those regions afterwards. |
| `sanitize.h`, `sanitize.cc` | `INK_Sanitize`. It removes metadata, attached files, scripts, thumbnails and private application data from every object of a document. |
| `inspect.h`, `inspect.cc` | `INK_Inspect_*`, for the redaction check. `INK_Inspect_Page` counts what lies under regions of a page, or finds candidate regions. `INK_Inspect_File` reports what a file carries besides its pages. Reports are JSON with numbers and booleans only, never text from the file. |
| `exports.txt` | The export allowlist. |
| `build.sh`, `link.sh` | The build. |
| `smoke.mjs` | The smoke check. |
| `sample.mjs` | Writes `inkbar/out/sample.pdf`, one annotation of each kind Inkbar writes, each with a hand-drawn appearance. Opening it in a viewer shows whether that viewer keeps the appearance or redraws its own. |
| `package.sh`, `write-manifest.sh` | The release archive and its manifest. |
| `size-budget.txt` | The size budget. |

## Build

The build runs in a container pinned to an Ubuntu 24.04 image by digest. The scripts find the repository root themselves, so they run from any directory. The commands below are run from the root.

```sh
inkbar/build.sh
node inkbar/smoke.mjs testing/resources/hello_world.pdf
node inkbar/sample.mjs
```

To check the redacted file the smoke check writes, set `SMOKE_REDACT_OUT` to a path inside the checkout and run `podman run --rm --security-opt label=disable -v "$PWD:/w" localhost/inkbar-qpdf:11 qpdf --check /w/<path>` on it.

`build.sh` needs an `origin` remote. It does not check the working tree: it builds the checkout as it is, uncommitted changes included. A release needs more. `write-manifest.sh` needs full history and an `upstream` remote, and records `HEAD` as the fork commit, so build a release from a clean tree.

The script uses `podman`. Set `CONTAINER=docker` to use Docker. The first run downloads several gigabytes of dependencies into the checkout. They are ignored by git. A clean build takes about ten minutes.

The checkout is mounted into the container. `scripts/embedpdf-runtime/build-target.sh wasm32` builds `libembedpdf.a`. `inkbar/link.sh` then compiles `inkbar/shim.c` and Inkbar's C++ files and links them with it into `inkbar/out/`. The C++ files use PDFium's internal classes, so the link step compiles them with the defines, include paths and flags GN gave PDFium's `fpdfsdk` sources. It reads them from `out/embedpdf-runtime/wasm32/obj/fpdfsdk/fpdfsdk.ninja`.

| File | Contents |
| --- | --- |
| `inkbar-pdfium.wasm` | The module. |
| `inkbar-pdfium.mjs` | An ES module. Its default export is the Emscripten factory. |
| `exports.txt` | A copy of the export allowlist. |

`inkbar/exports.txt` lists every exported function. The link step passes it to Emscripten as `EXPORTED_FUNCTIONS`, and no Inkbar source marks a function for export itself. `inkbar/smoke.mjs` instantiates the module and fails if its function exports differ from the list. `inkbar/size-budget.txt` holds the largest gzipped size of the `.wasm` file in bytes. The link step fails above it.

The binary is single-threaded.

Current size of `inkbar-pdfium.wasm`: 6,930,433 bytes raw, 2,928,196 bytes gzipped at level 9. The budget is 3,256,192 bytes gzipped.

## The redaction check

`INK_Inspect_Page` walks a page's objects as the redaction does and uses the redaction's own edge rules, so content the apply rightly kept does not count. For text and paths it also runs a second test that does not share the redaction's geometry, and a region counts content when either test finds it. The second tests use the region shrunk by 1 pt on every side, or by a quarter of its smaller side when that is less than 1 pt. A rotated region's edges each move that distance inward, perpendicular to themselves.

- Text: a character counts when its glyph box meets the region on an open interval, in the page content and in Form XObjects at any depth. In the second test, a character in the page content counts when the box PDFium's text extraction gives it (its tight and loose boxes together) meets the shrunk region. Text in annotation appearances and page copies, which text extraction does not read, is tested against a box built from the character's advance and the font's ascent and descent. The two counts are not merged character by character: the report gives the larger.
- Images: an image counts when the samples the apply would blank, the bounding box of the region on the image's sample grid, are not one value. The same test runs on its `/SMask` and on a stream `/Mask`, each on its own grid. The tolerance is 0 when the last filter is lossless. After `DCTDecode` or `JPXDecode` it is 12 levels either way per 8-bit channel. In a JPEG, samples inside a block that the region's edge crosses may spread over 48 levels (24 either way), because the apply re-encodes the blocks the edge crosses and the kept content beside the hole rings into them. Blocks are aligned to the image's top left and are 8 x 8, or the MCU when chroma is subsampled (16 x 16 for 4:2:0), read from the JPEG's frame header; a JPEG whose header cannot be read is taken to have 16 x 16 blocks. Every other sample is held to 12, in a region of any size. JPEG 2000 images (before an apply rewrites them as JPEG) are held to 12 throughout. This is a limit: in the blocks a region's edge crosses, the check does not see content within 48 levels of the fill value. On a 4:2:0 JPEG a region under 16 samples across lies wholly in such blocks. `/SMaskInData` in a JPEG 2000 image and colour-key `/Mask` arrays are not checked. When regions are given, blank means the value the apply fills a hole with: the samples upstream's redaction writes (`RedactionHoleSamples()`, the colour space's paper for an image, unpainted for a stencil, painted for a stream `/Mask`, opaque for a `/SMask`), decoded as a one-sample image with the same colour space, depth and `/Decode`. A sample counts when it is more than 12 levels from that value, or more than 48 inside a crossed JPEG block. So a solid image that is not the fill counts. Without regions any one flat value is blank. An image that cannot be decoded counts, and so does one whose fill value cannot be known. Each image is decoded once per call; decoded samples past 64 MB are released, least recently used first.
- Paths and shadings: one counts when PDFium's path redactor would cut it, tested against the region shrunk by 0.01 pt, because the apply cuts a hair outside the region and a save rounds coordinates. A shading counts only when its clip paths meet the region too. In the second test, a path counts when a vertex, or a point sampled along its segments at most 1 pt apart, lies inside the shrunk region. In the second test for a shading, points sampled at most 1 pt apart inside the shrunk region are each tested against the shading's box and every clip path with its own fill rule, curves flattened to within 0.25 pt on the page (the matrix's Frobenius norm bounds the stretch); a point the shading still paints counts it. The points are taken a row at a time: each clip's edges are crossed with the row's line once, and the sorted crossings say which of the row's points the clip covers. So a shading costs rows times edges plus points. Both tests read only a shading's box, its clip paths and its matrix, so shadings alike in all three (one painted again and again under one clip) are tested once per region.
- When regions are given, a form whose dictionary has `/INKB_Overlay true` is an overlay an apply drew. Nothing inside it counts.
- What an object carries with it counts when the object itself paints in the region: the cell of a tiling pattern it paints with, a shading pattern that can hold a picture, and the group of a soft mask on it. An object paints in a region when the box of its shape, grown by its stroke, meets the region shrunk by 0.01 pt, every clip in effect reaches it (the object's own and those of the form objects it is painted through, each by the redactor's test), and so does its own shape (a path by the redactor's test and the sampler, filled text by its glyphs' boxes, stroked text by its grown box, an image by its quad). Each part is tested against the region on its own, so the answer errs towards yes.
- An object paints with the colours it uses: a path with its fill colour when it is filled and its stroke colour when it is stroked; text by its rendering mode (fill for modes 0, 2, 4 and 6, stroke for 1, 2, 5 and 6, neither for 3 and 7); an image only when it is a stencil mask, with the fill colour.
- For such an object, a tiling pattern's cell counts whole, wherever in the cell things lie: its characters and images as pattern content, and one path when it holds a path, a shading or an object painted with a pattern of its own. Patterns used inside the cell are followed. This covers patterns in annotation appearances and page copies (text and images only, as for their other content). A cell is read once in a call, wherever it is used, and nothing is parsed once the work budget is spent.
- For such an object, the group of a soft mask (an ExtGState `/SMask` with a `/G` form) is read through the matrix in effect when the mask was set, and its text, images, paths and shadings count by the same rules as the content around the object, in the same fields. A mask set on the page and inherited by the content of a form painted under it lies where it was set; the form's matrix does not move it again. A group in effect on many objects through one matrix is read once, a group that masks its own content is read once, and a group whose place is not finite or that cannot be read counts.
- A plain ramp is an axial or radial shading (types 2 and 3) whose function is an exponential function (type 2) or a stitch (type 3) of at most 8 of them. The part of a plain ramp that an apply leaves in view shows all of it. A path filled with one is a path: it counts when the redactor would cut it, and not after an apply cut it. Painted with `sh`, it keeps the rule above.
- Any other shading can hold a picture and counts as a path: a function-based shading (type 1), a mesh (types 4 to 7), and an axial or radial shading with a sampled or calculated function or a longer stitch. Painted with `sh`, it counts when its own box meets the region shrunk by 0.01 pt, whatever clips it, so it still counts in a file where something clipped it away from the region. Its own box is its `/BBox` cut to the bounds of its mesh's points (vertices, or all of a patch's control points) or, for type 1, to its `/Domain` under its `/Matrix`, placed by the matrix it is painted through; without either it is taken to paint everywhere. Used as a shading pattern, it counts for an object that paints in the region. The check fails closed: a shading of any other type number, a pattern that does not load, a mesh that cannot be read to its end, and a box that is not finite all count. Reading a mesh is charged to the budget below, once per shading in a call.
- A Type 3 glyph counts as a character when its procedure's own bounds meet the region though the box it declares does not. The procedure is read from the font's own dictionary, not from PDFium's glyph cache, which drops a bitmap glyph's parsed form once a page was rendered. A procedure that cannot be read is taken to reach the region.
- A stream that cannot be read counts. PDFium falls back on the bytes it holds when a stream's filters fail, and a parse of those finds nothing where another reader may decode the stream or part of it. So a mask group, a pattern cell, a glyph procedure or an appearance whose filters left every byte as it was and in which no object was found counts as holding content. Streams of 16 bytes or fewer are not asked.
- Annotation appearances count text and images when the annotation's `/Rect` meets the region. Every state of `/N`, `/D` and `/R` is read. A form with `/INKB_PageCopy true` (a loupe's copy of its page, made by `INK_Appearance_AddPage`) is read in the page's own coordinates through the inverse of its `/Matrix`, wherever the annotation sits. `INK_Redact_ApplyPage` drops the cached copy of the page it changes, so a later `INK_Appearance_AddPage` copies the redacted page.

Every region, given or found, is cut to the page box grown by 10 pt before anything is sampled: a redact annotation's quads come from the file and may be any size. A region wholly outside has nothing under it. Every sampling loop is capped:

- at most 2^22 points per object under a region;
- at most 2^20 steps along one length, whether a side of a region or the part of a line inside it. A longer length cannot be sampled 1 pt apart;
- a shading's clips are flattened only when their control points' box meets the region, each curve split at most 8 times and each clip into at most 65,536 points.

Meeting a cap counts the object as present, and so does a number that is not finite: a coordinate, the matrix, or the box of a shading or of one of its clips on the page, which finite numbers can overflow. The check fails closed.

One `INK_Inspect_Page` call also has a work budget of 2^30 units for its sampling tests, over all objects and regions. A unit is one point tried, one clip edge crossed with a row of points, one crossing sorted (counted 16 times), or one clip point read or flattened, one byte of the page's content, a mask group, a pattern cell, a glyph procedure or an appearance parsed, and one object of it read. A unit takes about two nanoseconds with the parsing and the redactor's test around it, so the budget runs out after about two seconds: the smoke check's page of 8,000 clipped shadings under a page-sized mark stops in 2.1 s. An ordinary page uses a small part of it. Over PDFium's 419 sample pages, inspected for candidates and under a page-sized region, the most any call used was 1,257 units, and the smoke check's ordinary pages use at most 5,454. When the budget runs out the call returns NULL.

`INK_Inspect_Page` returns NULL when it could not inspect: no such page, a bad quad, a page whose box is not finite, a form nested more than 40 deep (PDFium's own limit, past which it leaves the form unparsed and empty though another viewer may draw it; the same holds inside an appearance or a pattern), patterns painted with patterns more than 64 deep, a region PDFium's path redactor cannot use, or a page that needs more sampling work than the budget allows. NULL never means that nothing was found; a caller treats it as a failed check. A region too small to shrink is tested as it is.

### What the apply cannot rewrite

The apply removes text, blanks image samples and cuts paths. It does not enter a tiling pattern's cell, a shading's definition, a soft mask's group or a Type 3 glyph procedure. It cuts or clips the object that paints with one of these and leaves the carrier in the file, where removing the cut brings back what it holds under the mark. After the apply the cut object no longer says what it covered, so the saved file cannot answer whether a carrier was painted under the mark.

So the question is asked before the apply, on the page as it is, and twice. `INK_Redact_Unremovable` reports the counts for the regions a caller gives, so the engine can name the marks it refuses. `INK_Redact_ApplyPage` asks the same of the marks it is about to apply, the given ones and the own marks alike, and returns -1 without changing the page when anything counts or it could not look. A caller cannot apply over a carrier by leaving the first call out.

For each region the count is of the objects, in the page content and in forms at any depth, that paint in the region as defined above and are:

- `patterns`: painting with a tiling pattern, whatever its cell holds;
- `shadings`: a shading that can hold a picture (not a plain ramp) painted with `sh`, or an object painting with such a shading pattern, when the shading's own box also meets the region or cannot be read;
- `maskGroups`: under a soft mask whose group holds anything whose box meets the region when placed by the mask's matrix. A group whose place is not finite or that cannot be read counts. A group that draws nothing but plain ramps and filled axis-aligned rectangles does not: that is how gradient transparency is drawn, and it holds no picture. Text, an image, any other path, a shading that can hold a picture, a pattern, a form or a mask of its own in the group makes it count. A mask a form's content inherits is counted once, at the form object;
- `glyphs`: text in a Type 3 font with a shown glyph whose procedure's bounds meet the region though its declared box does not, or whose procedure cannot be read. The text redactor goes by the declared box, so a glyph whose box meets the region is removed with its text, and its procedure with it when nothing else shows that glyph, whatever the procedure holds: an image (bitmap fonts draw every glyph so), a pattern or a shading. Such text is not counted.

One case does not count for `patterns`, `shadings` and `maskGroups`: an object whose own shape lies wholly inside one region, 0.01 pt clear of its edge. The apply removes such an object outright, the page no longer names what it carried, and the rewrite does not write it. The shape is the object's box with its stroke, clips aside; for a shading painted with `sh` it is the shading's own box. Clips do not help: a shape that reaches outside the region and is clipped to a window inside it is cut, not removed, and counts. A form is emptied and an image is blanked, and both stay on the page, so a mask on a form and a pattern on a stencil mask always count. An out-of-box glyph always counts. The smoke check reads the saved file for each kind wholly inside a mark (a pattern-filled path, pattern-coloured text, a pattern-stroked path, a mesh painted with `sh`, a mesh shading-pattern fill, a function-based shading, a masked object whose group holds text): the cell, the mesh, the function and the group's text are gone. When another object elsewhere on the page still paints with the same pattern, the pattern stays in the file, because the page shows it there; the object under the mark is removed all the same.

Tags in the file are not trusted before an apply: forms tagged `/INKB_Overlay` or `/INKB_PageCopy` are read like any other. Annotation appearances are not read: the apply removes every annotation whose `/Rect` meets a region, and one whose `/Rect` does not is drawn inside that rectangle and cannot paint in the region.

NULL from `INK_Redact_Unremovable` means it could not look: no such page, a bad quad, forms nested past PDFium's limit of 40, or the work budget spent. The page's own content is charged to the budget by its streams' stored bytes and its objects. A mask group, a pattern cell, a glyph procedure and an appearance are charged before they are parsed, by their decoded length, and by their objects after.

`INK_Inspect_Page` verifies what an apply removed. It is not the gate for these carriers. In candidate mode it answers what is painted under a box. It does not find the content of a pattern, a mask group or a shading pattern whose painted area was cut away from the box, as in a file another tool redacted by cutting a hole in a pattern fill and drawing a box into it: the cut fill no longer paints under the box, and the saved file cannot say that it once did. The smoke check pins this as a known limit. A shading painted with `sh` is the exception and still counts under a clip.

Limits of both: they answer what is under the mark. They do not find content hidden elsewhere in the file, such as a second use of a form that is clipped away, covered or off the page, the span of a gradient that no object shows, or definitions shared with other pages. The painted-area test reads each clip and the object's shape against the region separately, and takes text that clips to let everything through, so it can count an object that a combination of clips keeps out of the region.

When the only use of a resource lies wholly under a mark, the apply removes the object, upstream's content generation drops the page's resource entry, and the rewrite does not write the definition. The smoke check reads the saved file for each kind: an image and a form holding text (each leaves a blank copy in place, without the samples or the text), a tiling pattern, a mesh shading, a shading pattern, a soft mask group holding text, and an image named by resources the page inherits from its parent. The smoke check also pins what an apply leaves of other content painted through a reference. Gone from the saved file: a Type 3 glyph procedure used only under the mark, text in a transparency group form, text on a hidden layer, replacement text (`/ActualText`, `/Alt`) on a span, in `/Properties` and on a structure element, a field's value, a note and its popup, an attached file, an image's alternate, metadata and private data, a page's thumbnail and private data, and text used as a clip. Kept, because the page still shows it beside the mark: a glyph procedure, a form or an appearance stream that has another use there.

With no regions, it lists candidates: every redact annotation; filled rectangles in the page content at least 4 pt in each direction, opaque, with normal blending and a fill luminance below 0.2 or above 0.98; and Square, Highlight and Stamp annotations whose normal appearance is one such opaque filled rectangle. A candidate counts only content painted before it, in content order with forms taken at their place. Annotations paint after the content, in `/Annots` order. So a white page background has nothing under it, and text drawn on top of a box is not under the box. Without regions, `/INKB_Overlay` is not trusted and content in tagged forms counts. A filled box with nothing under it may be a real redaction or a graphic.

Known limits of the check:

- The shading sampler has no smoke check of its own that stays in place: a shading the sampler catches is also caught by the redactor's test, so the checks prove it only with that test turned off by hand.
- The work budget covers the sampling tests only. The redactor's own test of a path or a clip runs Skia's path operations, and its time grows with the square of the path's points: about 6 s for one clip of 60,000 points whose box meets a region, on the machine that measured the budget. The redactor also reports a ring of 1,000 points or more as crossing the region in its hole. That errs on the safe side, but the time has no bound. Identical shadings are tested once; distinct paths and clips are each tested.
- An image with both a `/SMask` and a stream `/Mask` is counted after an apply, because upstream's decoded path does not redact that `/Mask`; the check then refuses the apply.
- A very low-quality JPEG may not decode its flat fill within 12 levels of white, and the check then counts it after an apply.

`INK_Inspect_File` counts revisions as one more than the `%%EOF` markers a further `startxref` follows. A file counts as linearized only when its first object is a linearization dictionary (the key is exactly `/Linearized`) whose `/L` is the offset just past a `%%EOF`, the one that ends the linearized part, and whose first trailer's `/Prev` points forward past its own `%%EOF` (an update's points back). Then the earlier `%%EOF` that ends its first-page section is not counted, so a linearized file is one revision and one with an appended update is two. A file whose first object only claims to be linearized counts every revision. `info` is true for a non-empty `/Info`, whether the trailer names an object or holds a direct dictionary.

`INK_Sanitize` also removes the `/XFA` of an interactive form and `/URI` actions whose target starts with `javascript:` in any case. It removes the `/EF` of every file specification, those of GoToE, Launch, RichMedia, Screen and Sound included. Each embedded file counts as one attachment, however many places refer to it. A file specification that names a file without embedding it counts as one attachment too, whether an `/EmbeddedFiles` entry or a file attachment annotation (or both) refers to it, and a file attachment annotation with no file specification counts as one. A `/URI` target is read as a browser reads it: a UTF-16BE or UTF-16LE string is decoded, leading control characters and spaces are stripped, and tabs and line breaks are dropped wherever they are. A second `INK_Sanitize` on the same document finds nothing.

## Changes outside `inkbar/`

Every change this fork's Inkbar commits make outside `inkbar/`. Update this list with any further such change, and re-check each entry after every upstream merge.

- `core/fpdfdoc/cpdf_interactiveform.cpp`, in `InitDict`: a created `/AcroForm` is given an empty `/Fields` array. ISO 32000 requires the entry, and without it Poppler reports "Can't get Fields array". The smoke check guards it.
- `core/fpdfapi/edit/cpdf_image_redactor.{h,cpp}`: `RedactionHoleSamples()`, a public wrapper over the file's own `HoleSamples()` and `ColorSpaceOf()`, so the redaction check compares an image with the value the redaction fills it with. The smoke check guards it.
- `core/fpdfapi/parser/cpdf_document.{h,cpp}`: `CPDF_Document::DropInfo()`, which `INK_Sanitize` calls so the next save writes no `/Info`. It caches a direct, empty `/Info` dictionary and removes `/Info` from the parser's trailer, so the save trailer names none and copies none, whether the file named an object or held a direct dictionary. A layer document shares its base's parser, so on a layer it changes only the cached `/Info` and leaves the base's trailer alone. The smoke check guards both cases.
- `core/fpdfapi/edit/cpdf_pagecontentgenerator.cpp`, in `RealizeResource`: a direct resource, such as a shading or pattern dictionary held inside `/Resources`, is copied into an object of its own before the rewritten content names it. Upstream writes a reference to object 0, which the save drops, so a shading beside a redaction disappeared from the page. The smoke check guards it.

## Taking upstream

Take upstream once a month. Also take it out of cycle when a Chrome stable release notes list a PDFium security fix.

1. Fetch and merge: `git fetch upstream main && git merge upstream/main`. Resolve conflicts, keeping changes outside `inkbar/` small.
2. Rebuild with `inkbar/build.sh` and run `node inkbar/smoke.mjs testing/resources/hello_world.pdf`.
3. Run the `@inkbar/engine` tests against the new archive. Do this before the monorepo's lock file moves to the new release.
4. Push `main`, then push a tag named `v<branch>.<n>`. `<branch>` is the highest Chromium branch number that is an ancestor of `HEAD`. `<n>` counts releases on that branch, starting at 1. The release workflow builds the module and publishes the archive.

The `upstream` workflow runs every Monday. It writes the number of upstream commits not yet merged to its job summary. It also runs the freshness check, and it never fails for being behind.

## Releases

A tag that starts with `v` triggers the release workflow. It builds the module and publishes `inkbar-pdfium-wasm32-<tag>.tar.gz` to a GitHub release. The release notes give the upstream commit, the Chromium branch, the gzipped size of the module and the SHA-256 of the archive.

The archive holds seven files: `inkbar-pdfium.wasm`, `inkbar-pdfium.mjs`, `exports.txt`, `manifest.json`, `LICENSE`, `NOTICE` and `THIRD_PARTY_LICENSES`. `manifest.json` records the tag, the fork and upstream commits, the Chromium branch, the Emscripten version, the GN arguments and the module's SHA-256 and gzipped size.

`inkbar/package.sh` writes `THIRD_PARTY_LICENSES`. It reads Ninja's dependency log for the objects in `libembedpdf.a` and collects every directory under `third_party/` that their sources and headers come from. For each one it copies the files named on the `License File` line of its `README.chromium` or `README.pdfium`. It then adds the licences of Emscripten and of the C library, C++ library and compiler runtime that Emscripten links in. The script fails when a library has no licence file, so a new dependency cannot ship without its licence. It needs GNU tar, so it runs on Linux.

`inkbar/write-manifest.sh` fails when the tag's branch part differs from the Chromium branch it finds.

## Licence

Apache License, Version 2.0. See `LICENSE`. `NOTICE` records the origin of this repository and the copyright of each contributor.
