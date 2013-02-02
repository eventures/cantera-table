#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>

#include "ca-table.h"

#define MAX_HEADER_SIZE 64

static void
CA_put_integer (uint8_t **output, uint64_t value)
{
  uint8_t buffer[10];
  unsigned int ptr = 9;

  buffer[ptr] = value & 0x7f;
  value >>= 7;

  while (value)
    {
      buffer[--ptr] = 0x80 | value;

      value >>= 7;
    }

  memcpy (*output, &buffer[ptr], 10 - ptr);

  *output += 10 - ptr;
}

static void
CA_put_float (uint8_t **output, float value)
{
  memcpy (*output, &value, sizeof (value));

  *output += sizeof (value);
}

int
ca_table_write_time_float4 (struct ca_table *table, const char *key,
                            uint64_t start_time, uint32_t interval,
                            const float *sample_values, size_t sample_count)
{
  struct iovec value[3];
  uint8_t header[MAX_HEADER_SIZE], *o;

  o = header;

  CA_put_integer (&o, start_time);
  CA_put_integer (&o, interval);
  CA_put_integer (&o, sample_count);

  value[0].iov_base = (void *) key;
  value[0].iov_len = strlen (key) + 1;
  value[1].iov_base = header;
  value[1].iov_len = o - header;
  value[2].iov_base = (void *) sample_values;
  value[2].iov_len = sizeof (*sample_values) * sample_count;

  return ca_table_insert_row (table,
                              value, sizeof (value) / sizeof (value[0]));
}

int
ca_table_write_offset_score (struct ca_table *table, const char *key,
                             const struct ca_offset_score *values,
                             size_t count)
{
  struct iovec iov[2];

  uint8_t *target, *o;
  size_t i, target_alloc, target_size = 0;
  uint64_t prev_offset = 0;

  int result = -1;

  target_alloc = 32;

  if (!(target = ca_malloc (target_alloc)))
    return -1;

  o = target;

  CA_put_integer (&o, CA_OFFSET_SCORE_VARBYTE_FLOAT);
  CA_put_integer (&o, count);

  for (i = 0; i < count; ++i)
    {
      target_size = o - target;

      if (target_size + 16 > target_alloc)
        {
          if (-1 == CA_ARRAY_GROW (&target, &target_alloc))
            goto done;

          o = target + target_size;
        }

      CA_put_integer (&o, values[i].offset - prev_offset);
      prev_offset = values[i].offset;

      CA_put_float (&o, values[i].score);
    }

  iov[0].iov_base = (void *) key;
  iov[0].iov_len = strlen (key) + 1;

  iov[1].iov_base = target;
  iov[1].iov_len = o - target;

  result = ca_table_insert_row (table, iov, sizeof (iov) / sizeof (iov[0]));

done:

  free (target);

  return result;
}
