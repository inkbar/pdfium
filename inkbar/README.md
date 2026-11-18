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
| `redact.h`, `redact.cc` | `INK_Redact_*`. `INK_Redact_SetOverlay` gives a redact annotation its overlay, tagged `/INKB_Overlay true`. `INK_Redact_ApplyPage` applies a page's redact annotations through `EPDFPage_ApplyRedactions` and regenerates the page content. |
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

Current size of `inkbar-pdfium.wasm`: 6,886,865 bytes raw, 2,911,694 bytes gzipped at level 9. The budget is 3,256,192 bytes gzipped.

## The redaction check

`INK_Inspect_Page` walks a page's objects as the redaction does and uses the redaction's own edge rules, so content the apply rightly kept does not count. For text and paths it also runs a second test that does not share the redaction's geometry, and a region counts content when either test finds it. The second tests use the region shrunk by 1 pt on every side, or by a quarter of its smaller side when that is less than 1 pt. A rotated region's edges each move that distance inward, perpendicular to themselves.

- Text: a character counts when its glyph box meets the region on an open interval, in the page content and in Form XObjects at any depth. In the second test, a character in the page content counts when the box PDFium's text extraction gives it (its tight and loose boxes together) meets the shrunk region. Text in annotation appearances and page copies, which text extraction does not read, is tested against a box built from the character's advance and the font's ascent and descent. The two counts are not merged character by character: the report gives the larger.
- Images: an image counts when the samples the apply would blank, the bounding box of the region on the image's sample grid, are not one value. The same test runs on its `/SMask` and on a stream `/Mask`, each on its own grid. The tolerance is 0 when the last filter is lossless. After `DCTDecode` or `JPXDecode` it is 12 levels either way per 8-bit channel. In a JPEG, samples inside a block that the region's edge crosses may spread over 48 levels (24 either way), because the apply re-encodes the blocks the edge crosses and the kept content beside the hole rings into them. Blocks are aligned to the image's top left and are 8 x 8, or the MCU when chroma is subsampled (16 x 16 for 4:2:0), read from the JPEG's frame header; a JPEG whose header cannot be read is taken to have 16 x 16 blocks. Every other sample is held to 12, in a region of any size. JPEG 2000 images (before an apply rewrites them as JPEG) are held to 12 throughout. `/SMaskInData` in a JPEG 2000 image and colour-key `/Mask` arrays are not checked. When regions are given, blank means the value the apply fills a hole with: the samples upstream's redaction writes (`RedactionHoleSamples()`, the colour space's paper for an image, unpainted for a stencil, painted for a stream `/Mask`, opaque for a `/SMask`), decoded as a one-sample image with the same colour space, depth and `/Decode`. A sample counts when it is more than 12 levels from that value, or more than 48 inside a crossed JPEG block. So a solid image that is not the fill counts. Without regions any one flat value is blank. An image that cannot be decoded counts, and so does one whose fill value cannot be known. Each image is decoded once per call; decoded samples past 64 MB are released, least recently used first.
- Paths and shadings: one counts when PDFium's path redactor would cut it, tested against the region shrunk by 0.01 pt, because the apply cuts a hair outside the region and a save rounds coordinates. A shading counts only when its clip paths meet the region too. In the second test, a path counts when a vertex, or a point sampled along its segments at most 1 pt apart, lies inside the shrunk region. In the second test for a shading, points sampled at most 1 pt apart inside the shrunk region are each tested against the shading's box and every clip path with its own fill rule; a point the shading still paints counts it.
- When regions are given, a form whose dictionary has `/INKB_Overlay true` is an overlay an apply drew. Nothing inside it counts.
- Text and images in the content stream of a tiling pattern that an object paints into the region count as pattern content. This includes patterns in annotation appearances and in page copies, and patterns used inside another pattern's content. The redaction does not enter patterns, so this content survives an apply. Vector drawing inside a pattern is not counted.
- Annotation appearances count text and images when the annotation's `/Rect` meets the region. Every state of `/N`, `/D` and `/R` is read. A form with `/INKB_PageCopy true` (a loupe's copy of its page, made by `INK_Appearance_AddPage`) is read in the page's own coordinates through the inverse of its `/Matrix`, wherever the annotation sits. `INK_Redact_ApplyPage` drops the cached copy of the page it changes, so a later `INK_Appearance_AddPage` copies the redacted page.

Every region, given or found, is cut to the page box grown by 10 pt before anything is sampled: a redact annotation's quads come from the file and may be any size. A region wholly outside has nothing under it. Every sampling loop is capped, so no value in a file can make it run without end.

`INK_Inspect_Page` returns NULL when it could not inspect: no such page, a bad quad, a form nested more than 40 deep (PDFium's own limit, past which it leaves the form unparsed and empty though another viewer may draw it; the same holds inside an appearance or a pattern), patterns painted with patterns more than 64 deep, or a region PDFium's path redactor cannot use. NULL never means that nothing was found; a caller treats it as a failed check. A region too small to shrink is tested as it is.

With no regions, it lists candidates: every redact annotation; filled rectangles in the page content at least 4 pt in each direction, opaque, with normal blending and a fill luminance below 0.2 or above 0.98; and Square, Highlight and Stamp annotations whose normal appearance is one such opaque filled rectangle. A candidate counts only content painted before it, in content order with forms taken at their place. Annotations paint after the content, in `/Annots` order. So a white page background has nothing under it, and text drawn on top of a box is not under the box. Without regions, `/INKB_Overlay` is not trusted and content in tagged forms counts. A filled box with nothing under it may be a real redaction or a graphic.

`INK_Inspect_File` counts revisions as one more than the `%%EOF` markers a further `startxref` follows. A file counts as linearized only when its first object is a linearization dictionary (the key is exactly `/Linearized`) whose `/L` is the offset just past a `%%EOF`, the one that ends the linearized part. Then the earlier `%%EOF` that ends its first-page section is not counted, so a linearized file is one revision and one with an appended update is two. A file whose first object only claims to be linearized counts every revision. `info` is true for a non-empty `/Info`, whether the trailer names an object or holds a direct dictionary.

`INK_Sanitize` also removes the `/XFA` of an interactive form and `/URI` actions whose target starts with `javascript:` in any case. It removes the `/EF` of every file specification, those of GoToE, Launch, RichMedia, Screen and Sound included. Each embedded file counts as one attachment, however many places refer to it. A file specification that names a file without embedding it counts as one attachment too, whether an `/EmbeddedFiles` entry or a file attachment annotation (or both) refers to it, and a file attachment annotation with no file specification counts as one. A `/URI` target is read as a browser reads it: a UTF-16BE string is decoded, leading control characters and spaces are stripped, and tabs and line breaks are dropped wherever they are. A second `INK_Sanitize` on the same document finds nothing.

## Changes outside `inkbar/`

Every change this fork's Inkbar commits make outside `inkbar/`. Update this list with any further such change, and re-check each entry after every upstream merge.

- `core/fpdfdoc/cpdf_interactiveform.cpp`, in `InitDict`: a created `/AcroForm` is given an empty `/Fields` array. ISO 32000 requires the entry, and without it Poppler reports "Can't get Fields array". The smoke check guards it.
- `core/fpdfapi/edit/cpdf_image_redactor.{h,cpp}`: `RedactionHoleSamples()`, a public wrapper over the file's own `HoleSamples()` and `ColorSpaceOf()`, so the redaction check compares an image with the value the redaction fills it with. The smoke check guards it.
- `core/fpdfapi/parser/cpdf_document.{h,cpp}`: `CPDF_Document::DropInfo()`, which `INK_Sanitize` calls so the next save writes no `/Info`. It caches a direct, empty `/Info` dictionary and removes `/Info` from the parser's trailer, so the save trailer names none and copies none, whether the file named an object or held a direct dictionary. A layer document shares its base's parser, so on a layer it changes only the cached `/Info` and leaves the base's trailer alone. The smoke check guards both cases.

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
