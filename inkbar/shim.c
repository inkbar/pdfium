// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Glue that a JavaScript host needs and PDFium's C API leaves to the embedder.
// inkbar/exports.txt alone decides which of these functions the module exports.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fpdf_formfill.h"
#include "fpdf_save.h"
#include "fpdfview.h"

typedef struct {
  FPDF_FILEWRITE base;
  unsigned char* data;
  size_t size;
  size_t cap;
} InkWriter;

static int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  InkWriter* w = (InkWriter*)self;
  if (size > SIZE_MAX - w->size)
    return 0;
  size_t need = w->size + size;
  if (need > w->cap) {
    size_t cap = w->cap ? w->cap : 65536;
    while (cap < need) {
      if (cap > SIZE_MAX / 2)
        return 0;
      cap *= 2;
    }
    unsigned char* grown = realloc(w->data, cap);
    if (!grown)
      return 0;
    w->data = grown;
    w->cap = cap;
  }
  memcpy(w->data + w->size, data, size);
  w->size += size;
  return 1;
}

void INK_Init(void) {
  FPDF_LIBRARY_CONFIG config;
  memset(&config, 0, sizeof(config));
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);
}

void INK_Destroy(void) {
  FPDF_DestroyLibrary();
}

// Returns NULL on failure.
InkWriter* INK_Save(FPDF_DOCUMENT doc, unsigned long flags) {
  InkWriter* w = calloc(1, sizeof(InkWriter));
  if (!w)
    return NULL;
  w->base.version = 1;
  w->base.WriteBlock = WriteBlock;
  if (!FPDF_SaveAsCopy(doc, &w->base, flags)) {
    free(w->data);
    free(w);
    return NULL;
  }
  return w;
}

unsigned char* INK_WriterData(InkWriter* w) {
  return w->data;
}

size_t INK_WriterSize(InkWriter* w) {
  return w->size;
}

void INK_WriterFree(InkWriter* w) {
  if (!w)
    return;
  free(w->data);
  free(w);
}

// Version 1: AcroForm only, no XFA, no JavaScript platform.
FPDF_FORMFILLINFO* INK_NewFormFillInfo(void) {
  FPDF_FORMFILLINFO* info = calloc(1, sizeof(FPDF_FORMFILLINFO));
  if (info)
    info->version = 1;
  return info;
}

void INK_FreeFormFillInfo(FPDF_FORMFILLINFO* info) {
  free(info);
}
