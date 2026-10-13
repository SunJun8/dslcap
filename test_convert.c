/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 SunJ */

/* Unit tests for dslcap parsing and cross-format conversion. */

#include "convert.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

#define CHECK(cond)                                                        \
  do                                                                       \
    {                                                                      \
      if (!(cond))                                                         \
        {                                                                  \
          printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
          g_failed++;                                                      \
        }                                                                  \
    }                                                                      \
  while (0)

static void test_channels(void)
{
  uint32_t m;

  CHECK(dslcap_parse_channels("0-3,8", &m) == 0 && m == 0x10f);
  CHECK(dslcap_parse_channels("31", &m) == 0 && m == 0x80000000u);
  CHECK(dslcap_parse_channels("0-31", &m) == 0 && m == 0xffffffffu);
  CHECK(dslcap_parse_channels("32", &m) != 0);
  CHECK(dslcap_parse_channels("3-1", &m) != 0);
  CHECK(dslcap_parse_channels("", &m) != 0);
  CHECK(dslcap_parse_channels("1,", &m) != 0);
  CHECK(dslcap_parse_channels("a", &m) != 0);
  CHECK(dslcap_parse_channels("-1", &m) != 0);
}

static void test_numbers(void)
{
  uint64_t v;

  CHECK(dslcap_parse_si("100M", &v) == 0 && v == 100000000);
  CHECK(dslcap_parse_si("1.5k", &v) == 0 && v == 1500);
  CHECK(dslcap_parse_si("1G", &v) == 0 && v == 1000000000);
  CHECK(dslcap_parse_si("500", &v) == 0 && v == 500);
  CHECK(dslcap_parse_si("1.5", &v) != 0);
  CHECK(dslcap_parse_si("10x", &v) != 0);
  CHECK(dslcap_parse_si("M", &v) != 0);
  CHECK(dslcap_parse_si("-1", &v) != 0);

  CHECK(dslcap_parse_duration("10ms", &v) == 0 && v == 10000000);
  CHECK(dslcap_parse_duration("2s", &v) == 0 && v == 2000000000);
  CHECK(dslcap_parse_duration("1.5us", &v) == 0 && v == 1500);
  CHECK(dslcap_parse_duration("100ns", &v) == 0 && v == 100);
  CHECK(dslcap_parse_duration("10", &v) != 0);
  CHECK(dslcap_parse_duration("10m", &v) != 0);
}

struct sink
{
  uint8_t buf[256 * 4];
  size_t samples;
  unsigned int unitsize;
  size_t stop_after;
};

static int sink_emit(const uint8_t *samples, size_t count, void *arg)
{
  struct sink *s = arg;

  memcpy(s->buf + s->samples * s->unitsize, samples, count * s->unitsize);
  s->samples += count;
  return s->stop_after != 0 && s->samples >= s->stop_after;
}

/* Hand-written block for channels 0 and 2: channel 0 is high at samples 0 and
 * 63, channel 2 is high at sample 1 and 8. The analyzer channel number must
 * become the bit number in the output, not the enabled-channel order. */
static void test_cross_known_block(void)
{
  static const uint8_t block[16] = {
    0x01, 0, 0, 0, 0, 0, 0, 0x80, /* channel 0 */
    0x02, 0x01, 0, 0, 0, 0, 0, 0, /* channel 2 */
  };
  struct dslcap_cross c;
  struct sink s;
  size_t i;

  memset(&s, 0, sizeof(s));
  s.unitsize = 1;
  CHECK(dslcap_cross_init(&c, 0x5, 1) == 0);
  CHECK(dslcap_cross_feed(&c, block, sizeof(block), sink_emit, &s) == 0);
  CHECK(s.samples == 64);
  for (i = 0; i < 64; i++)
    {
      uint8_t want = 0;

      if (i == 0 || i == 63)
        want = 0x01;
      else if (i == 1 || i == 8)
        want = 0x04;
      CHECK(s.buf[i] == want);
    }
}

/* USB transfers end at arbitrary byte offsets, so feeding the same stream in
 * odd-sized pieces must give the same samples as feeding it at once. */
static void test_cross_split_feed(void)
{
  uint8_t stream[3 * 8 * 4];
  struct dslcap_cross c;
  struct sink whole;
  struct sink split;
  size_t i;

  for (i = 0; i < sizeof(stream); i++)
    stream[i] = (uint8_t)(i * 37 + 11);

  memset(&whole, 0, sizeof(whole));
  whole.unitsize = 3;
  CHECK(dslcap_cross_init(&c, (1u << 1) | (1u << 9) | (1u << 17), 3) == 0);
  CHECK(dslcap_cross_feed(&c, stream, sizeof(stream), sink_emit, &whole) == 0);
  CHECK(whole.samples == 4 * 64);

  memset(&split, 0, sizeof(split));
  split.unitsize = 3;
  CHECK(dslcap_cross_init(&c, (1u << 1) | (1u << 9) | (1u << 17), 3) == 0);
  for (i = 0; i < sizeof(stream); i += 5)
    {
      size_t n = sizeof(stream) - i < 5 ? sizeof(stream) - i : 5;

      CHECK(dslcap_cross_feed(&c, stream + i, n, sink_emit, &split) == 0);
    }
  CHECK(split.samples == whole.samples);
  CHECK(memcmp(split.buf, whole.buf, whole.samples * 3) == 0);

  /* Bits outside the enabled channels stay zero. */
  for (i = 0; i < whole.samples * 3; i++)
    CHECK((whole.buf[i] & ~0x02) == 0);
}

static void test_cross_stop_and_limits(void)
{
  uint8_t stream[8 * 3];
  struct dslcap_cross c;
  struct sink s;

  memset(stream, 0xff, sizeof(stream));
  memset(&s, 0, sizeof(s));
  s.unitsize = 1;
  s.stop_after = 64;
  CHECK(dslcap_cross_init(&c, 0x1, 1) == 0);
  CHECK(dslcap_cross_feed(&c, stream, sizeof(stream), sink_emit, &s) == 1);
  CHECK(s.samples == 64);

  CHECK(dslcap_cross_init(&c, 0, 1) != 0);
  CHECK(dslcap_cross_init(&c, 1u << 8, 1) != 0);
  CHECK(dslcap_cross_init(&c, 1u << 31, 4) == 0);
}

int main(void)
{
  test_channels();
  test_numbers();
  test_cross_known_block();
  test_cross_split_feed();
  test_cross_stop_and_limits();
  if (g_failed == 0)
    printf("all tests passed\n");
  return g_failed != 0;
}
