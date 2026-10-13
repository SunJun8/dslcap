/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 SunJ */

/* Parsing helpers and DSLogic cross-format conversion for dslcap. */

#ifndef DSLCAP_CONVERT_H
#define DSLCAP_CONVERT_H

#include <stddef.h>
#include <stdint.h>

#define DSLCAP_MAX_CHANNELS  32
#define DSLCAP_BLOCK_SAMPLES 64

/* "0-3,8,10-11" -> bit mask of analyzer channels. Returns 0 on success. */
int dslcap_parse_channels(const char *s, uint32_t *mask);

/* "100M", "1.5k", "2G", "1000" (decimal SI suffix, integer result). */
int dslcap_parse_si(const char *s, uint64_t *value);

/* "10ms", "2s", "500us", "100ns" -> nanoseconds. */
int dslcap_parse_duration(const char *s, uint64_t *ns);

/* Receives converted samples: count samples of unitsize bytes each, bit k of
 * a sample is analyzer channel k. A non-zero return stops the feed. */
typedef int (*dslcap_emit_fn)(const uint8_t *samples, size_t count,
                              void *arg);

/* DSLogic sends logic data as blocks of 64 samples: for each enabled channel
 * in ascending index order, 8 bytes where sample s is bit (s % 8) of byte
 * (s / 8). USB packets may end anywhere inside a block. */
struct dslcap_cross
{
  unsigned int en_count;
  uint8_t en_index[DSLCAP_MAX_CHANNELS];
  unsigned int unitsize;
  size_t fill;
  uint8_t block[DSLCAP_MAX_CHANNELS * 8];
  uint8_t out[DSLCAP_BLOCK_SAMPLES * 4];
};

int dslcap_cross_init(struct dslcap_cross *c, uint32_t mask,
                      unsigned int unitsize);
int dslcap_cross_feed(struct dslcap_cross *c, const uint8_t *data,
                      size_t len, dslcap_emit_fn emit, void *arg);

#endif
