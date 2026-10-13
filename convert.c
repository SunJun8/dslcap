/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 SunJ */

/* Parsing helpers and DSLogic cross-format conversion for dslcap. */

#include "convert.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int dslcap_parse_channels(const char *s, uint32_t *mask)
{
  uint32_t m = 0;
  const char *p = s;

  if (s == NULL || *s == '\0')
    return -EINVAL;

  for (;;)
    {
      char *end;
      unsigned long lo;
      unsigned long hi;

      if (!isdigit((unsigned char)*p))
        return -EINVAL;
      lo = strtoul(p, &end, 10);
      hi = lo;
      p = end;
      if (*p == '-')
        {
          p++;
          if (!isdigit((unsigned char)*p))
            return -EINVAL;
          hi = strtoul(p, &end, 10);
          p = end;
        }
      if (lo > hi || hi >= DSLCAP_MAX_CHANNELS)
        return -EINVAL;
      for (; lo <= hi; lo++)
        m |= 1u << lo;
      if (*p == '\0')
        break;
      if (*p != ',')
        return -EINVAL;
      p++;
    }

  *mask = m;
  return 0;
}

/* Parses "<digits>[.<digits>]" and scales it by mult; the result must be an
 * integer. */
static int parse_scaled(const char *s, const char **rest, uint64_t mult,
                        uint64_t *value)
{
  uint64_t ip = 0;
  uint64_t frac = 0;
  uint64_t div = 1;
  const char *p = s;

  if (!isdigit((unsigned char)*p))
    return -EINVAL;
  for (; isdigit((unsigned char)*p); p++)
    {
      if (ip > (UINT64_MAX - 9) / 10)
        return -ERANGE;
      ip = ip * 10 + (uint64_t)(*p - '0');
    }
  if (*p == '.')
    {
      p++;
      if (!isdigit((unsigned char)*p))
        return -EINVAL;
      for (; isdigit((unsigned char)*p); p++)
        {
          if (div >= 1000000000ull)
            return -EINVAL;
          frac = frac * 10 + (uint64_t)(*p - '0');
          div *= 10;
        }
    }

  *rest = p;
  if (ip > UINT64_MAX / mult || (frac * mult) % div != 0)
    return -EINVAL;
  *value = ip * mult + frac * mult / div;
  return 0;
}

int dslcap_parse_si(const char *s, uint64_t *value)
{
  static const struct
  {
    char suffix;
    uint64_t mult;
  } units[] = {
    {'\0', 1}, {'k', 1000}, {'K', 1000}, {'M', 1000000},
    {'G', 1000000000ull},
  };
  const char *p = s;
  const char *rest;
  size_t i;

  if (s == NULL)
    return -EINVAL;
  while (isdigit((unsigned char)*p) || *p == '.')
    p++;
  for (i = 0; i < sizeof(units) / sizeof(units[0]); i++)
    {
      if (*p == units[i].suffix && (*p == '\0' || p[1] == '\0'))
        {
          uint64_t v;
          int ret = parse_scaled(s, &rest, units[i].mult, &v);

          if (ret < 0)
            return ret;
          if (rest != p)
            return -EINVAL;
          *value = v;
          return 0;
        }
    }
  return -EINVAL;
}

int dslcap_parse_duration(const char *s, uint64_t *ns)
{
  static const struct
  {
    const char *unit;
    uint64_t mult;
  } units[] = {
    {"ns", 1}, {"us", 1000}, {"ms", 1000000}, {"s", 1000000000ull},
  };
  const char *p = s;
  const char *rest;
  size_t i;

  if (s == NULL)
    return -EINVAL;
  while (isdigit((unsigned char)*p) || *p == '.')
    p++;
  for (i = 0; i < sizeof(units) / sizeof(units[0]); i++)
    {
      if (strcmp(p, units[i].unit) == 0)
        {
          uint64_t v;
          int ret = parse_scaled(s, &rest, units[i].mult, &v);

          if (ret < 0)
            return ret;
          if (rest != p)
            return -EINVAL;
          *ns = v;
          return 0;
        }
    }
  return -EINVAL;
}

int dslcap_cross_init(struct dslcap_cross *c, uint32_t mask,
                      unsigned int unitsize)
{
  unsigned int ch;

  if (mask == 0 || unitsize == 0 || unitsize > 4)
    return -EINVAL;

  memset(c, 0, sizeof(*c));
  c->unitsize = unitsize;
  for (ch = 0; ch < DSLCAP_MAX_CHANNELS; ch++)
    {
      if ((mask & (1u << ch)) == 0)
        continue;
      if (ch >= unitsize * 8)
        return -EINVAL;
      c->en_index[c->en_count++] = (uint8_t)ch;
    }
  return 0;
}

static void cross_convert(struct dslcap_cross *c, const uint8_t *block)
{
  unsigned int j;

  memset(c->out, 0, DSLCAP_BLOCK_SAMPLES * c->unitsize);
  for (j = 0; j < c->en_count; j++)
    {
      const uint8_t *src = block + j * 8;
      unsigned int ch = c->en_index[j];
      uint8_t *dst = c->out + ch / 8;
      uint8_t bit = (uint8_t)(1u << (ch % 8));
      unsigned int s;

      for (s = 0; s < DSLCAP_BLOCK_SAMPLES; s++)
        {
          if (src[s / 8] & (1u << (s % 8)))
            dst[s * c->unitsize] |= bit;
        }
    }
}

int dslcap_cross_feed(struct dslcap_cross *c, const uint8_t *data,
                      size_t len, dslcap_emit_fn emit, void *arg)
{
  size_t block_size = (size_t)c->en_count * 8;
  int ret;

  while (len > 0)
    {
      const uint8_t *block;

      if (c->fill == 0 && len >= block_size)
        {
          block = data;
          data += block_size;
          len -= block_size;
        }
      else
        {
          size_t n = block_size - c->fill;

          if (n > len)
            n = len;
          memcpy(c->block + c->fill, data, n);
          c->fill += n;
          data += n;
          len -= n;
          if (c->fill < block_size)
            break;
          c->fill = 0;
          block = c->block;
        }

      cross_convert(c, block);
      ret = emit(c->out, DSLCAP_BLOCK_SAMPLES, arg);
      if (ret != 0)
        return ret;
    }
  return 0;
}
