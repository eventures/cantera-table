#include <string.h>

#include "ca-table.h"

void
ca_format_integer (uint8_t **output, uint64_t value)
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


void
ca_format_float (uint8_t **output, float value)
{
  memcpy (*output, &value, sizeof (value));

  *output += sizeof (value);
}

void
ca_format_time_float4 (uint8_t **output,
                       uint64_t start_time, uint32_t interval,
                       const float *sample_values, size_t sample_count)
{
  uint8_t *o;

  o = *output;

  ca_format_integer (&o, start_time);
  ca_format_integer (&o, interval);
  ca_format_integer (&o, sample_count);

  memcpy (o, sample_values, sizeof (*sample_values) * sample_count);
  o += sizeof (*sample_values) * sample_count;

  *output = o;
}

void
ca_format_offset_score (uint8_t **output,
                        const struct ca_offset_score *values, size_t count)
{
  uint8_t *o;
  size_t i;
  uint64_t prev_offset = 0;

  o = *output;

  ca_format_integer (&o, CA_OFFSET_SCORE_VARBYTE_FLOAT);
  ca_format_integer (&o, count);

  for (i = 0; i < count; ++i)
    {
      ca_format_integer (&o, values[i].offset - prev_offset);
      prev_offset = values[i].offset;

      ca_format_float (&o, values[i].score);
    }

  *output = o;
}
