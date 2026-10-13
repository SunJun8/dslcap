/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 SunJ */

/* dslcap: command line capture for DreamSourceLab DSLogic analyzers, built on
 * DSView's libsigrok4DSL. Output is raw sample-major binary that sigrok-cli
 * reads with "-I binary:numchannels=N:samplerate=R". */

#include <errno.h>
#include <getopt.h>
#include <glib.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libsigrok4DSL/libsigrok.h"

#include "convert.h"

#define EXIT_USAGE    1
#define EXIT_NODEV    2
#define EXIT_CAPTURE  3

struct mode_info
{
  int id;
  const char *name;
  unsigned int channels;
  unsigned int max_enabled;
  uint64_t min_rate;
  uint64_t max_rate;
};

struct capture
{
  FILE *out;
  struct dslcap_cross cross;
  unsigned int unitsize;
  uint64_t want;
  uint64_t written;
  uint64_t trigger_pos;
  int trigger_enabled;
  int triggered;
  int error;
  int overflow;
  int write_error;
};

static struct capture g_cap;
static volatile int g_done;
static volatile int g_end_event;
static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig)
{
  (void)sig;
  g_interrupted = 1;
}

static void on_event(int event)
{
  if (event == DS_EV_COLLECT_TASK_END ||
      event == DS_EV_COLLECT_TASK_END_BY_ERROR ||
      event == DS_EV_COLLECT_TASK_END_BY_DETACHED)
    {
      __atomic_store_n(&g_end_event, event, __ATOMIC_SEQ_CST);
      __atomic_store_n(&g_done, 1, __ATOMIC_SEQ_CST);
    }
}

static int emit_samples(const uint8_t *samples, size_t count, void *arg)
{
  struct capture *cap = arg;
  uint64_t left = cap->want - cap->written;

  if (count > left)
    count = (size_t)left;
  if (count > 0 && fwrite(samples, cap->unitsize, count, cap->out) != count)
    {
      cap->write_error = errno;
      return -1;
    }
  cap->written += count;
  return cap->written >= cap->want;
}

static void on_data(const struct sr_dev_inst *sdi,
                    const struct sr_datafeed_packet *packet)
{
  struct capture *cap = &g_cap;

  (void)sdi;

  switch (packet->type)
    {
    case SR_DF_TRIGGER:
      if (packet->status != SR_PKT_OK)
        cap->error = 1;
      else if (cap->trigger_enabled &&
               ((const struct ds_trigger_pos *)packet->payload)->status & 1)
        {
          cap->triggered = 1;
          cap->trigger_pos =
            ((const struct ds_trigger_pos *)packet->payload)->real_pos;
        }
      break;

    case SR_DF_LOGIC:
      {
        const struct sr_datafeed_logic *logic = packet->payload;

        if (packet->status != SR_PKT_OK || logic->data_error ||
            logic->format != LA_CROSS_DATA)
          {
            cap->error = 1;
            break;
          }
        if (cap->written < cap->want && cap->write_error == 0)
          dslcap_cross_feed(&cap->cross, logic->data, logic->length,
                            emit_samples, cap);
      }
      break;

    case SR_DF_OVERFLOW:
      cap->overflow = 1;
      break;

    case SR_DF_END:
      if (packet->status != SR_PKT_OK)
        cap->error = 1;
      break;

    default:
      break;
    }
}

static int set_i16(int key, int16_t v)
{
  return ds_set_actived_device_config(NULL, NULL, key, g_variant_new_int16(v));
}

static int set_u64(int key, uint64_t v)
{
  return ds_set_actived_device_config(NULL, NULL, key,
                                      g_variant_new_uint64(v));
}

static int get_i16(int key, int16_t *v)
{
  GVariant *gv = NULL;

  if (ds_get_actived_device_config(NULL, NULL, key, &gv) != SR_OK || !gv)
    return -1;
  *v = g_variant_get_int16(gv);
  g_variant_unref(gv);
  return 0;
}

static int get_u64(int key, uint64_t *v)
{
  GVariant *gv = NULL;

  if (ds_get_actived_device_config(NULL, NULL, key, &gv) != SR_OK || !gv)
    return -1;
  *v = g_variant_get_uint64(gv);
  g_variant_unref(gv);
  return 0;
}

/* Samplerates allowed in the current channel mode, ascending. */
static const uint64_t *list_rates(GVariant **hold, gsize *count)
{
  GVariant *dict = NULL;
  GVariant *arr;
  const uint64_t *rates;

  *hold = NULL;
  *count = 0;
  if (ds_get_actived_device_config_list(NULL, SR_CONF_SAMPLERATE, &dict) !=
        SR_OK || !dict)
    return NULL;
  arr = g_variant_lookup_value(dict, "samplerates", G_VARIANT_TYPE("at"));
  g_variant_unref(dict);
  if (!arr)
    return NULL;
  rates = g_variant_get_fixed_array(arr, count, sizeof(uint64_t));
  *hold = arr;
  return rates;
}

/* Fills the channel modes of the current operation mode; switches the
 * device through each of them to read its channel count and rate range. */
static int list_modes(struct mode_info *modes, int max)
{
  GVariant *gv = NULL;
  const struct sr_list_item *item;
  int n = 0;
  int i;

  if (ds_get_actived_device_config_list(NULL, SR_CONF_CHANNEL_MODE, &gv) !=
        SR_OK || !gv)
    return -1;
  item = (const struct sr_list_item *)(uintptr_t)g_variant_get_uint64(gv);
  g_variant_unref(gv);

  for (; item && item->id >= 0 && n < max; item++, n++)
    {
      modes[n].id = item->id;
      modes[n].name = item->name;
    }

  for (i = 0; i < n; i++)
    {
      GVariant *hold;
      gsize count;
      const uint64_t *rates;
      int16_t vld;

      if (set_i16(SR_CONF_CHANNEL_MODE, (int16_t)modes[i].id) != SR_OK ||
          get_i16(SR_CONF_VLD_CH_NUM, &vld) != 0)
        return -1;
      modes[i].channels = g_slist_length(ds_get_actived_device_channels());
      modes[i].max_enabled = (unsigned int)vld;
      rates = list_rates(&hold, &count);
      if (!rates || count == 0)
        return -1;
      modes[i].min_rate = rates[0];
      modes[i].max_rate = rates[count - 1];
      g_variant_unref(hold);
    }
  return n;
}

static char *rate_str(uint64_t rate, char *buf, size_t len)
{
  if (rate >= UINT64_C(1000000000) && rate % UINT64_C(1000000000) == 0)
    snprintf(buf, len, "%" PRIu64 "G", rate / UINT64_C(1000000000));
  else if (rate >= 1000000 && rate % 1000000 == 0)
    snprintf(buf, len, "%" PRIu64 "M", rate / 1000000);
  else if (rate >= 1000 && rate % 1000 == 0)
    snprintf(buf, len, "%" PRIu64 "k", rate / 1000);
  else
    snprintf(buf, len, "%" PRIu64, rate);
  return buf;
}

static int open_device(int wait_ms)
{
  int elapsed;

  for (elapsed = 0;; elapsed += 200)
    {
      struct ds_device_base_info *list = NULL;
      struct ds_device_base_info *p;
      ds_device_handle handle = NULL_HANDLE;
      char name[sizeof(list->name)];
      int count = 0;

      if (ds_get_device_list(&list, &count) == SR_OK && list)
        {
          for (p = list; p->handle != NULL_HANDLE; p++)
            {
              if (strstr(p->name, "DSLogic"))
                {
                  handle = p->handle;
                  snprintf(name, sizeof(name), "%s", p->name);
                  break;
                }
            }
          free(list);
        }

      if (handle != NULL_HANDLE)
        {
          int status = 0;

          if (ds_active_device(handle) != SR_OK ||
              ds_get_actived_device_init_status(&status) != SR_OK ||
              status != SR_ST_ACTIVE)
            {
              fprintf(stderr, "dslcap: failed to open %s (error %d)\n", name,
                      ds_get_last_error());
              return -1;
            }
          fprintf(stderr, "dslcap: using %s\n", name);
          return 0;
        }
      if (elapsed >= wait_ms)
        {
          fprintf(stderr, "dslcap: no DSLogic device found\n");
          return -1;
        }
      g_usleep(200000);
    }
}

static int cmd_info(void)
{
  static const struct
  {
    int16_t op;
    const char *name;
  } ops[] = {
    {LO_OP_BUFFER, "buffer"},
    {LO_OP_STREAM, "stream"},
  };
  size_t i;

  for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
    {
      struct mode_info modes[16];
      int n;
      int j;

      if (set_i16(SR_CONF_OPERATION_MODE, ops[i].op) != SR_OK)
        return EXIT_CAPTURE;
      n = list_modes(modes, 16);
      if (n < 0)
        return EXIT_CAPTURE;
      printf("%s mode:\n", ops[i].name);
      for (j = 0; j < n; j++)
        {
          char lo[24];
          char hi[24];
          uint64_t depth = 0;

          set_i16(SR_CONF_CHANNEL_MODE, (int16_t)modes[j].id);
          get_u64(SR_CONF_HW_DEPTH, &depth);
          printf("  mode %-3d ch 0-%-2u max %2u on, %5s..%-5s Hz, "
                 "depth/ch %" PRIu64 "  (%s)\n",
                 modes[j].id, modes[j].channels - 1, modes[j].max_enabled,
                 rate_str(modes[j].min_rate, lo, sizeof(lo)),
                 rate_str(modes[j].max_rate, hi, sizeof(hi)), depth,
                 modes[j].name);
        }
    }
  return 0;
}

struct capture_opts
{
  const char *out;
  uint32_t mask;
  uint64_t rate;
  uint64_t samples;
  uint64_t duration_ns;
  double vth;
  int stream;
  int trigger_ch;
  char trigger_type;
  int trigger_pos;
  double timeout;
};

static int write_meta(const struct capture_opts *o, unsigned int unitsize)
{
  char path[4096];
  FILE *f;
  int ch;
  int first = 1;

  snprintf(path, sizeof(path), "%s.json", o->out);
  f = fopen(path, "w");
  if (!f)
    return -1;
  fprintf(f, "{\n  \"format\": \"raw-sample-major\",\n");
  fprintf(f, "  \"samplerate\": %" PRIu64 ",\n", o->rate);
  fprintf(f, "  \"samples\": %" PRIu64 ",\n", g_cap.written);
  fprintf(f, "  \"unitsize\": %u,\n", unitsize);
  fprintf(f, "  \"numchannels\": %u,\n", unitsize * 8);
  fprintf(f, "  \"channels\": [");
  for (ch = 0; ch < DSLCAP_MAX_CHANNELS; ch++)
    {
      if (o->mask & (1u << ch))
        {
          fprintf(f, "%s%d", first ? "" : ", ", ch);
          first = 0;
        }
    }
  fprintf(f, "],\n  \"vth\": %.2f,\n", o->vth);
  fprintf(f, "  \"mode\": \"%s\",\n", o->stream ? "stream" : "buffer");
  if (g_cap.triggered)
    fprintf(f, "  \"trigger_sample\": %" PRIu64 "\n}\n", g_cap.trigger_pos);
  else
    fprintf(f, "  \"trigger_sample\": null\n}\n");
  return fclose(f);
}

static int pick_mode(const struct capture_opts *o)
{
  struct mode_info modes[16];
  unsigned int need = 0;
  int n;
  int i;

  for (i = 0; i < DSLCAP_MAX_CHANNELS; i++)
    {
      if (o->mask & (1u << i))
        need = (unsigned int)i + 1;
    }

  n = list_modes(modes, 16);
  if (n < 0)
    {
      fprintf(stderr, "dslcap: cannot list channel modes\n");
      return -1;
    }
  for (i = 0; i < n; i++)
    {
      if (modes[i].channels >= need &&
          modes[i].max_enabled >= (unsigned int)__builtin_popcount(o->mask) &&
          modes[i].max_rate >= o->rate &&
          modes[i].min_rate <= o->rate)
        {
          if (set_i16(SR_CONF_CHANNEL_MODE, (int16_t)modes[i].id) != SR_OK)
            return -1;
          fprintf(stderr, "dslcap: channel mode %d (%s)\n", modes[i].id,
                  modes[i].name);
          return 0;
        }
    }
  fprintf(stderr,
          "dslcap: no %s channel mode fits channels up to %u (%d enabled) "
          "at this samplerate; run 'dslcap info'\n",
          o->stream ? "stream" : "buffer", need - 1,
          __builtin_popcount(o->mask));
  return -1;
}

static int configure(const struct capture_opts *o)
{
  GSList *l;
  GVariant *hold;
  const uint64_t *rates;
  gsize count;
  gsize i;
  uint64_t v;
  uint64_t limit;

  if (set_i16(SR_CONF_OPERATION_MODE,
              o->stream ? LO_OP_STREAM : LO_OP_BUFFER) != SR_OK)
    return -1;
  if (pick_mode(o) < 0)
    return -1;

  for (l = ds_get_actived_device_channels(); l; l = l->next)
    {
      const struct sr_channel *ch = l->data;

      ds_enable_device_channel(ch, (o->mask >> ch->index) & 1);
    }

  rates = list_rates(&hold, &count);
  for (i = 0; rates && i < count && rates[i] != o->rate; i++)
    ;
  if (!rates || i == count)
    {
      char buf[24];

      fprintf(stderr, "dslcap: samplerate %s not supported; valid:",
              rate_str(o->rate, buf, sizeof(buf)));
      for (i = 0; rates && i < count; i++)
        fprintf(stderr, " %s", rate_str(rates[i], buf, sizeof(buf)));
      fprintf(stderr, "\n");
      if (hold)
        g_variant_unref(hold);
      return -1;
    }
  g_variant_unref(hold);

  if (set_u64(SR_CONF_SAMPLERATE, o->rate) != SR_OK ||
      get_u64(SR_CONF_SAMPLERATE, &v) != 0 || v != o->rate)
    {
      fprintf(stderr, "dslcap: failed to set samplerate\n");
      return -1;
    }

  /* The device rounds a buffer capture down to whole 1024-sample units;
   * ask for the next unit and drop the extra samples on output. */
  limit = (o->samples + SAMPLES_ALIGN) & ~SAMPLES_ALIGN;
  if (!o->stream && get_u64(SR_CONF_HW_DEPTH, &v) == 0 && limit > v)
    {
      fprintf(stderr,
              "dslcap: %" PRIu64 " samples exceed buffer depth %" PRIu64
              " for %d channels\n",
              o->samples, v, __builtin_popcount(o->mask));
      return -1;
    }
  if (set_u64(SR_CONF_LIMIT_SAMPLES, limit) != SR_OK)
    {
      fprintf(stderr, "dslcap: failed to set sample limit\n");
      return -1;
    }

  if (ds_set_actived_device_config(NULL, NULL, SR_CONF_VTH,
                                   g_variant_new_double(o->vth)) != SR_OK)
    {
      fprintf(stderr, "dslcap: failed to set threshold\n");
      return -1;
    }

  ds_trigger_reset();
  if (o->trigger_ch >= 0)
    {
      ds_trigger_set_mode(SIMPLE_TRIGGER);
      ds_trigger_set_pos((uint16_t)o->trigger_pos);
      ds_trigger_set_en(1);
      ds_trigger_probe_set((uint16_t)o->trigger_ch, o->trigger_type, 'X');
    }
  else
    ds_trigger_set_en(0);
  return 0;
}

static double now_s(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmd_capture(struct capture_opts *o)
{
  unsigned int top = 0;
  double start;
  int stopped = 0;
  int ch;
  int ret = 0;

  for (ch = 0; ch < DSLCAP_MAX_CHANNELS; ch++)
    {
      if (o->mask & (1u << ch))
        top = (unsigned int)ch;
    }

  memset(&g_cap, 0, sizeof(g_cap));
  g_cap.unitsize = top / 8 + 1;
  g_cap.want = o->samples;
  g_cap.trigger_enabled = o->trigger_ch >= 0;
  if (dslcap_cross_init(&g_cap.cross, o->mask, g_cap.unitsize) != 0)
    return EXIT_USAGE;

  if (configure(o) < 0)
    return EXIT_CAPTURE;

  g_cap.out = fopen(o->out, "wb");
  if (!g_cap.out)
    {
      fprintf(stderr, "dslcap: %s: %s\n", o->out, strerror(errno));
      return EXIT_CAPTURE;
    }

  if (o->timeout <= 0)
    o->timeout = (double)o->samples / (double)o->rate + 10.0;

  g_done = 0;
  g_end_event = 0;
  start = now_s();
  if (ds_start_collect() != SR_OK)
    {
      fprintf(stderr, "dslcap: failed to start capture\n");
      fclose(g_cap.out);
      return EXIT_CAPTURE;
    }

  while (!__atomic_load_n(&g_done, __ATOMIC_SEQ_CST))
    {
      if (g_interrupted || now_s() - start > o->timeout)
        {
          fprintf(stderr, "dslcap: %s, stopping\n",
                  g_interrupted ? "interrupted" : "timeout");
          if (ds_is_collecting())
            ds_stop_collect();
          stopped = 1;
          break;
        }
      g_usleep(20000);
    }

  if (fclose(g_cap.out) != 0 && g_cap.write_error == 0)
    g_cap.write_error = errno;
  write_meta(o, g_cap.unitsize);

  if (g_cap.write_error)
    {
      fprintf(stderr, "dslcap: write %s: %s\n", o->out,
              strerror(g_cap.write_error));
      ret = EXIT_CAPTURE;
    }
  if (g_cap.error || g_cap.overflow ||
      __atomic_load_n(&g_end_event, __ATOMIC_SEQ_CST) !=
        DS_EV_COLLECT_TASK_END)
    {
      if (!stopped)
        fprintf(stderr, "dslcap: capture failed (event %d%s%s)\n",
                g_end_event, g_cap.error ? ", data error" : "",
                g_cap.overflow ? ", overflow" : "");
      ret = EXIT_CAPTURE;
    }
  if (g_cap.written < g_cap.want)
    {
      fprintf(stderr, "dslcap: only %" PRIu64 " of %" PRIu64
              " samples captured\n", g_cap.written, g_cap.want);
      ret = EXIT_CAPTURE;
    }

  {
    char buf[24];

    printf("captured %" PRIu64 " samples at %sHz -> %s (unitsize %u)\n",
           g_cap.written, rate_str(o->rate, buf, sizeof(buf)), o->out,
           g_cap.unitsize);
    if (g_cap.triggered)
      printf("trigger at sample %" PRIu64 "\n", g_cap.trigger_pos);
    printf("decode: sigrok-cli -i %s -I binary:numchannels=%u:samplerate=%"
           PRIu64 " -P <decoder>\n",
           o->out, g_cap.unitsize * 8, o->rate);
  }
  return ret;
}

static void usage(FILE *f)
{
  fprintf(f,
"usage: dslcap [-v] [--fw-dir DIR] info\n"
"       dslcap [-v] [--fw-dir DIR] capture -o FILE [options]\n"
"\n"
"capture options:\n"
"  -o, --output FILE      raw sample-major output; metadata in FILE.json\n"
"  -c, --channels LIST    analyzer channels, e.g. 0-7,16 (default 0-15)\n"
"  -r, --samplerate RATE  e.g. 100M, 10M, 500k (default 100M)\n"
"  -n, --samples N        e.g. 1M (default 1M)\n"
"  -t, --time DURATION    capture length instead of -n, e.g. 20ms\n"
"      --vth VOLT         input threshold (default 1.6)\n"
"      --stream           stream mode (default buffer mode)\n"
"      --trigger CH:TYPE  TYPE r=rising f=falling e=either edge 0/1=level\n"
"      --pos PERCENT      pre-trigger part of the capture (default 10)\n"
"      --timeout SEC      give up after SEC (default capture time + 10)\n"
"\n"
"Bit k of each output sample is analyzer channel k; sigrok-cli names it\n"
"<k> with -I binary:numchannels=8*unitsize:samplerate=RATE, e.g.\n"
"-P uart:rx=17:baudrate=2000000.\n");
}

int main(int argc, char **argv)
{
  static const struct option long_opts[] = {
    {"output", required_argument, NULL, 'o'},
    {"channels", required_argument, NULL, 'c'},
    {"samplerate", required_argument, NULL, 'r'},
    {"samples", required_argument, NULL, 'n'},
    {"time", required_argument, NULL, 't'},
    {"vth", required_argument, NULL, 'V'},
    {"stream", no_argument, NULL, 'S'},
    {"trigger", required_argument, NULL, 'T'},
    {"pos", required_argument, NULL, 'P'},
    {"timeout", required_argument, NULL, 'W'},
    {"fw-dir", required_argument, NULL, 'F'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };
  struct capture_opts o;
  const char *fw_dir = DSLCAP_FW_DIR;
  const char *cmd;
  int verbose = 0;
  int opt;
  int ret;

  memset(&o, 0, sizeof(o));
  o.mask = 0xffff;
  o.rate = 100000000;
  o.samples = 1000000;
  o.vth = 1.6;
  o.trigger_ch = -1;
  o.trigger_pos = 10;

  while ((opt = getopt_long(argc, argv, "o:c:r:n:t:vh", long_opts, NULL)) !=
         -1)
    {
      char *end;

      switch (opt)
        {
        case 'o':
          o.out = optarg;
          break;
        case 'c':
          if (dslcap_parse_channels(optarg, &o.mask) != 0)
            {
              fprintf(stderr, "dslcap: bad channel list '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 'r':
          if (dslcap_parse_si(optarg, &o.rate) != 0 || o.rate == 0)
            {
              fprintf(stderr, "dslcap: bad samplerate '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 'n':
          if (dslcap_parse_si(optarg, &o.samples) != 0 || o.samples == 0)
            {
              fprintf(stderr, "dslcap: bad sample count '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 't':
          if (dslcap_parse_duration(optarg, &o.duration_ns) != 0 ||
              o.duration_ns == 0)
            {
              fprintf(stderr, "dslcap: bad duration '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 'V':
          o.vth = strtod(optarg, &end);
          if (*end != '\0' || o.vth <= 0 || o.vth > 5)
            {
              fprintf(stderr, "dslcap: bad threshold '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 'S':
          o.stream = 1;
          break;
        case 'T':
          {
            static const char types[] = "rfe01";
            static const char codes[] = "RFC01";
            const char *t;

            o.trigger_ch = (int)strtol(optarg, &end, 10);
            if (end == optarg || *end != ':' || o.trigger_ch < 0 ||
                o.trigger_ch >= DSLCAP_MAX_CHANNELS || end[1] == '\0' ||
                end[2] != '\0' || (t = strchr(types, end[1])) == NULL)
              {
                fprintf(stderr, "dslcap: bad trigger '%s'\n", optarg);
                return EXIT_USAGE;
              }
            o.trigger_type = codes[t - types];
          }
          break;
        case 'P':
          o.trigger_pos = (int)strtol(optarg, &end, 10);
          if (*end != '\0' || o.trigger_pos < 0 || o.trigger_pos > 100)
            {
              fprintf(stderr, "dslcap: bad trigger position '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 'W':
          o.timeout = strtod(optarg, &end);
          if (*end != '\0' || o.timeout <= 0)
            {
              fprintf(stderr, "dslcap: bad timeout '%s'\n", optarg);
              return EXIT_USAGE;
            }
          break;
        case 'F':
          fw_dir = optarg;
          break;
        case 'v':
          verbose++;
          break;
        case 'h':
          usage(stdout);
          return 0;
        default:
          usage(stderr);
          return EXIT_USAGE;
        }
    }

  if (optind != argc - 1)
    {
      usage(stderr);
      return EXIT_USAGE;
    }
  cmd = argv[optind];
  if (strcmp(cmd, "capture") == 0)
    {
      if (!o.out)
        {
          fprintf(stderr, "dslcap: capture needs -o FILE\n");
          return EXIT_USAGE;
        }
      if (o.duration_ns)
        o.samples = (uint64_t)((double)o.rate * o.duration_ns / 1e9 + 0.5);
      if (o.trigger_ch >= 0 && (o.mask & (1u << o.trigger_ch)) == 0)
        {
          fprintf(stderr, "dslcap: trigger channel %d is not captured\n",
                  o.trigger_ch);
          return EXIT_USAGE;
        }
    }
  else if (strcmp(cmd, "info") != 0)
    {
      usage(stderr);
      return EXIT_USAGE;
    }

  signal(SIGINT, on_sigint);
  ds_log_level(verbose >= 2 ? 4 : verbose ? 3 : 1);
  ds_set_firmware_resource_dir(fw_dir);
  ds_set_event_callback(on_event);
  ds_set_datafeed_callback(on_data);
  if (ds_lib_init() != SR_OK)
    {
      fprintf(stderr, "dslcap: libsigrok4DSL init failed\n");
      return EXIT_NODEV;
    }

  if (open_device(3000) < 0)
    ret = EXIT_NODEV;
  else if (strcmp(cmd, "info") == 0)
    ret = cmd_info();
  else
    ret = cmd_capture(&o);

  ds_lib_exit();
  return ret;
}
