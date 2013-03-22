/*
    Database structure management routines
    Copyright (C) 2013    Morten Hustveit
    Copyright (C) 2013    eVenture Capital Partners II

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include "ca-table.h"
#include "query.h"

enum ca_schema_table_type
{
  CA_SCHEMA_TABLE_SUMMARY,
  CA_SCHEMA_TABLE_INDEX,
  CA_SCHEMA_TABLE_TIME_SERIES
};

struct ca_schema_table
{
  enum ca_schema_table_type type;
  char *path;
  uint64_t offset;
};

struct ca_schema
{
  char *path;

  struct ca_schema_table *tables;
  size_t table_alloc, table_count;

  struct ca_table **summary_tables;
  uint64_t *summary_table_offsets;
  size_t summary_table_count;

  struct ca_table **index_tables;
  size_t index_table_count;

  struct ca_table **time_series_tables;
  size_t time_series_table_count;
};

static int
CA_schema_load (struct ca_schema *schema)
{
  FILE *f;
  char line[4096];
  int lineno = 0, result = -1;

  if (!(f = fopen (schema->path, "r")))
    {
      ca_set_error ("Failed to open '%s' for reading: %s", schema->path, strerror (errno));

      return -1;
    }

  line[sizeof (line) - 1] = 0;

  while (NULL != fgets (line, sizeof (line), f))
    {
      struct ca_schema_table table;
      char *path, *offset_string;
      size_t line_length;

      ++lineno;

      if (line[sizeof (line) - 1])
        {
          ca_set_error ("%s:%d: Line too long.  Max is %zu",
                        lineno, schema->path, sizeof (line) - 1);

          goto fail;
        }

      line_length = strlen (line);

      /* fgets stores the \n, and we might just as well remove all trailing
       * whitespace */
      while (line_length && isspace (line[line_length - 1]))
        line[--line_length] = 0;

      if (!line[0] || line[0] == '#')
        continue;

      path = strchr (line, '\t');

      if (!path)
        {
          ca_set_error ("%s:%d: Missing TAB character", schema->path, lineno);

          goto fail;
        }

      *path++ = 0;
      offset_string = strchr (path, '\t');

      if (!strcmp (line, "summary"))
        {
          table.type = CA_SCHEMA_TABLE_SUMMARY;
          ++schema->summary_table_count;
        }
      else if (!strcmp (line, "index"))
        {
          table.type = CA_SCHEMA_TABLE_INDEX;
          ++schema->index_table_count;
        }
      else if (!strcmp (line, "time-series"))
        {
          table.type = CA_SCHEMA_TABLE_TIME_SERIES;
          ++schema->time_series_table_count;
        }
      else
        {
          ca_set_error ("%s:%d: Unknown table type \"%s\"", schema->path, lineno, line);

          goto fail;
        }

      if (-1 == access (path, R_OK))
        {
          ca_set_error ("%s:%d: Path \"%s\" is not readable: %s",
                        schema->path, lineno, path, strerror (errno));

          goto fail;
        }

      if (offset_string)
        {
          char *endptr;

          if (table.type != CA_SCHEMA_TABLE_SUMMARY)
            {
              ca_set_error ("%s:%d: Unexpected column for table type \"%s\"", line);

              goto fail;
            }

          table.offset = (uint64_t) strtoll (offset_string, &endptr, 0);

          if (*endptr)
            {
              ca_set_error ("%s:%d: Expected EOL after offset, got \\x%02x",
                            (unsigned char) *endptr);

              goto fail;
            }
        }
      else
        table.offset = 0;

      if (schema->table_count == schema->table_alloc
          && -1 == CA_ARRAY_GROW (&schema->tables, &schema->table_alloc))
        goto fail;

      if (!(table.path = ca_strdup (path)))
        goto fail;

      schema->tables[schema->table_count++] = table;
    }

  result = 0;

fail:

  fclose (f);

  return result;
}

struct ca_schema *
ca_schema_load (const char *path)
{
  struct ca_schema *result;

  int ok = 0;

  if (path[0] != '/')
    {
      ca_set_error ("Schema path must be absolute");

      return NULL;
    }

  if (path[strlen (path) - 1] == '/')
    {
      ca_set_error ("Schema path must not end with a slash (/)");

      return NULL;
    }

  if (!(result = ca_malloc (sizeof (*result))))
    return NULL;

  if (!(result->path = ca_strdup (path)))
    goto fail;

  if (-1 == CA_schema_load (result))
    goto fail;

  ok = 1;

fail:

  if (!ok)
    {
      ca_schema_close (result);
      result = NULL;
    }

  return result;
}

void
ca_schema_close (struct ca_schema *schema)
{
  size_t i;

  if (schema->time_series_tables)
    {
      for (i = 0; i < schema->time_series_table_count; ++i)
        ca_table_close (schema->time_series_tables[i]);
      free (schema->time_series_tables);
    }

  if (schema->index_tables)
    {
      for (i = 0; i < schema->index_table_count; ++i)
        ca_table_close (schema->index_tables[i]);
      free (schema->index_tables);
    }

  if (schema->summary_tables)
    {
      for (i = 0; i < schema->summary_table_count; ++i)
        ca_table_close (schema->summary_tables[i]);
      free (schema->summary_tables);
    }

  free (schema->summary_table_offsets);
  free (schema->tables);
  free (schema->path);
  free (schema);
}

ssize_t
ca_schema_summary_tables (struct ca_schema *schema,
                          struct ca_table ***tables,
                          uint64_t **offsets)
{
  if (!schema->summary_tables)
    {
      size_t i, j = 0;

      if (!(schema->summary_tables = ca_malloc (sizeof (*schema->summary_tables) * schema->summary_table_count)))
        return -1;

      if (!(schema->summary_table_offsets = ca_malloc (sizeof (*schema->summary_table_offsets) * schema->summary_table_count)))
        return -1;

      for (i = 0; i < schema->table_count; ++i)
        {
          if (schema->tables[i].type != CA_SCHEMA_TABLE_SUMMARY)
            continue;

          if (!(schema->summary_tables[j] = ca_table_open ("write-once", schema->tables[i].path, O_RDONLY, 0)))
            return -1;

          schema->summary_table_offsets[j] = schema->tables[i].offset;

          ++j;
        }
    }

  *tables = schema->summary_tables;
  *offsets = schema->summary_table_offsets;

  return schema->summary_table_count;
}

ssize_t
ca_schema_index_tables (struct ca_schema *schema,
                        struct ca_table ***tables)
{
  if (!schema->index_tables)
    {
      size_t i, j = 0;

      if (!(schema->index_tables = ca_malloc (sizeof (*schema->index_tables) * schema->index_table_count)))
        return -1;

      for (i = 0; i < schema->table_count; ++i)
        {
          if (schema->tables[i].type != CA_SCHEMA_TABLE_INDEX)
            continue;

          if (!(schema->index_tables[j] = ca_table_open ("write-once", schema->tables[i].path, O_RDONLY, 0)))
            return -1;

          ++j;
        }
    }

  *tables = schema->index_tables;

  return schema->index_table_count;
}

ssize_t
ca_schema_time_series_tables (struct ca_schema *schema,
                              struct ca_table ***tables)
{
  if (!schema->time_series_tables)
    {
      size_t i, j = 0;

      if (!(schema->time_series_tables = ca_malloc (sizeof (*schema->time_series_tables) * schema->time_series_table_count)))
        return -1;

      for (i = 0; i < schema->table_count; ++i)
        {
          if (schema->tables[i].type != CA_SCHEMA_TABLE_TIME_SERIES)
            continue;

          if (!(schema->time_series_tables[j] = ca_table_open ("write-once", schema->tables[i].path, O_RDONLY, 0)))
            return -1;

          ++j;
        }
    }

  *tables = schema->time_series_tables;

  return schema->time_series_table_count;
}
