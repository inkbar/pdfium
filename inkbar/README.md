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

`build.sh` needs an `origin` remote. It does not check the working tree: it builds the checkout as it is, uncommitted changes included. A release needs more. `write-manifest.sh` needs full history and an `upstream` remote, and records `HEAD` as the fork commit, so build a release from a clean tree.

The script uses `podman`. Set `CONTAINER=docker` to use Docker. The first run downloads several gigabytes of dependencies into the checkout. They are ignored by git. A clean build takes about ten minutes.

The checkout is mounted into the container. `scripts/embedpdf-runtime/build-target.sh wasm32` builds `libembedpdf.a`. `inkbar/link.sh` then compiles `inkbar/shim.c` and `inkbar/appearance.cc` and links them with it into `inkbar/out/`. `appearance.cc` uses PDFium's internal classes, so the link step compiles it with the defines, include paths and flags GN gave PDFium's `fpdfsdk` sources. It reads them from `out/embedpdf-runtime/wasm32/obj/fpdfsdk/fpdfsdk.ninja`.

| File | Contents |
| --- | --- |
| `inkbar-pdfium.wasm` | The module. |
| `inkbar-pdfium.mjs` | An ES module. Its default export is the Emscripten factory. |
| `exports.txt` | A copy of the export allowlist. |

`inkbar/exports.txt` lists every exported function. The link step passes it to Emscripten as `EXPORTED_FUNCTIONS`, and no Inkbar source marks a function for export itself. `inkbar/smoke.mjs` instantiates the module and fails if its function exports differ from the list. `inkbar/size-budget.txt` holds the largest gzipped size of the `.wasm` file in bytes. The link step fails above it.

The binary is single-threaded.

Current size of `inkbar-pdfium.wasm`: 5,932,318 bytes raw, 2,629,908 bytes gzipped at level 9. The budget is 3,256,192 bytes gzipped.

## Changes outside `inkbar/`

Every change this fork's Inkbar commits make outside `inkbar/`. Update this list with any further such change, and re-check each entry after every upstream merge.

- `core/fpdfdoc/cpdf_interactiveform.cpp`, in `InitDict`: a created `/AcroForm` is given an empty `/Fields` array. ISO 32000 requires the entry, and without it Poppler reports "Can't get Fields array". The smoke check guards it.

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
