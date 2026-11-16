# Inkbar PDFium

This is Inkbar's fork of PDFium. It is built for `@inkbar/engine`, the PDF engine behind Inkbar.

## Provenance

The history here comes from three sources, in order.

1. PDFium, from https://pdfium.googlesource.com/pdfium. The remote `upstream` points at it.
2. `embedpdf/runtime`, from https://github.com/embedpdf/runtime. It is an Apache-2.0 fork of PDFium that adds editing and WebAssembly work.
3. Inkbar's own commits, which follow the pinned commit.

The pinned commit is `f2ea7597287f26bdcb450602de6616ff6939d506` on the `embedpdf/main` branch of `embedpdf/runtime`. Everything after it in this repository's history is Inkbar's.

## Layout

Everything Inkbar adds lives under `inkbar/`. Changes to files outside that directory are kept small and are recorded in the commit history.

## Build

The build runs in a container pinned to an Ubuntu 24.04 image by digest. Run it from the repository root.

```sh
inkbar/build.sh
node inkbar/smoke.mjs testing/resources/hello_world.pdf
```

The build needs a full-history clone with an `origin` remote and a clean working tree.

The script uses `podman`. Set `CONTAINER=docker` to use Docker. The first run downloads several gigabytes of dependencies into the checkout. They are ignored by git. A clean build takes about ten minutes.

The checkout is mounted into the container. `scripts/embedpdf-runtime/build-target.sh wasm32` builds `libembedpdf.a`. `inkbar/link.sh` then links it with `inkbar/shim.c` into `inkbar/out/`.

| File | Contents |
| --- | --- |
| `inkbar-pdfium.wasm` | The module. |
| `inkbar-pdfium.mjs` | An ES module. Its default export is the Emscripten factory. |
| `exports.txt` | A copy of the export allowlist. |

`inkbar/exports.txt` lists every exported function. A function that is not on the list is not in the module. `inkbar/size-budget.txt` holds the largest gzipped size of the `.wasm` file in bytes. The link step fails above it.

The binary is single-threaded.

Current size of `inkbar-pdfium.wasm`: 5,750,585 bytes raw, 2,563,012 bytes gzipped at level 9. The budget is 3,256,192 bytes gzipped.

## Taking upstream

The process for taking in upstream PDFium changes is documented here once the release process lands.

## Licence

Apache License, Version 2.0. See `LICENSE`. `NOTICE` records the origin of this repository and the copyright of each contributor.
