#ifndef TABLE_H_
#define TABLE_H_ 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CA_USE_RESULT __attribute__((warn_unused_result))

#define CA_NAMEDATALEN 64

/*****************************************************************************/

const char *
ca_last_error (void);

void
ca_clear_error (void);

void
ca_set_error (const char *format, ...);

/*****************************************************************************/

enum ca_value_type
{
  CA_TEXT = 0,
  CA_TIME_SERIES = 1,
  CA_TABLE_DECLARATION = 2,
  CA_INT64 = 3,
  CA_NUMERIC = 4
};

enum ca_field_flag
{
  CA_FIELD_NOT_NULL =    0x0001,
  CA_FIELD_PRIMARY_KEY = 0x0002
};

struct ca_field
{
  char name[CA_NAMEDATALEN];
  uint32_t flags;
  uint16_t pad0;
  uint8_t pad1;
  uint8_t type; /* enum ca_value_type */
};

struct ca_table_declaration
{
  char *path;
  uint32_t field_count;
  struct ca_field *fields;
};

struct ca_data
{
  enum ca_value_type type;

  union
    {
      struct
        {
          uint64_t start_time;
          uint32_t interval;
          size_t count;
          const float *values;
        } time_series;

      struct ca_table_declaration table_declaration;
    } v;
};

/*****************************************************************************/

enum ca_table_flag
{
  CA_TABLE_NO_RELATIVE,
  CA_TABLE_NO_FSYNC
};

struct ca_table_backend
{
  void *
  (*open) (const char *path, int flags, mode_t mode);

  int
  (*sync) (void *handle);

  void
  (*close) (void *handle);

  int
  (*set_flag) (void *handle, enum ca_table_flag flag);

  int
  (*is_sorted) (void *handle);

  int
  (*insert_row) (void *handle, const char *key,
                 const struct iovec *value, size_t value_count);

  int
  (*seek) (void *handle, off_t offset, int whence);

  int
  (*seek_to_key) (void *handle, const char *key);

  off_t
  (*offset) (void *handle);

  ssize_t
  (*read_row) (void *handle, const char **key,
               struct iovec *value);

  int
  (*delete_row) (void *handle);
};

void
ca_table_register_backend (const char *name,
                           const struct ca_table_backend *backend);

struct ca_table_backend *
ca_table_backend (const char *name);

/*****************************************************************************/

struct ca_table;

struct ca_table *
ca_table_open (const char *backend_name,
               const char *path, int flags, mode_t mode) CA_USE_RESULT;

int
ca_table_sync (struct ca_table *table) CA_USE_RESULT;

void
ca_table_close (struct ca_table *table);

int
ca_table_set_flag (struct ca_table *table, enum ca_table_flag flag);

int
ca_table_is_sorted (struct ca_table *table) CA_USE_RESULT;

int
ca_table_insert_row (struct ca_table *table, const char *key,
                     const struct iovec *value, size_t value_count) CA_USE_RESULT;

int
ca_table_seek (struct ca_table *table, off_t offset, int whence) CA_USE_RESULT;

/**
 * Returns -1 on failure.  Use ca_last_error() to get an error description.
 * Returns 0 if the key was not found.
 * Returns 1 if the key was found.
 */
int
ca_table_seek_to_key (struct ca_table *table, const char *key) CA_USE_RESULT;

off_t
ca_table_offset (struct ca_table *table) CA_USE_RESULT;

ssize_t
ca_table_read_row (struct ca_table *table, const char **key,
                   struct iovec *value) CA_USE_RESULT;

int
ca_table_delete_row (struct ca_table *table) CA_USE_RESULT;

/*****************************************************************************/

struct ca_fifo;

struct ca_fifo *
ca_fifo_create (size_t size);

void
ca_fifo_free (struct ca_fifo *fifo);

void
ca_fifo_put (struct ca_fifo *fifo, const void *data, size_t size);

void
ca_fifo_get (struct ca_fifo *fifo, void *data, size_t size);

/*****************************************************************************/

struct ca_schema;

struct ca_schema *
ca_schema_load (const char *path) CA_USE_RESULT;

void
ca_schema_close (struct ca_schema *schema);

int
ca_schema_create_table (struct ca_schema *schema, const char *table_name,
                        struct ca_table_declaration *declaration) CA_USE_RESULT;

struct ca_table *
ca_schema_table (struct ca_schema *schema, const char *table_name,
                 struct ca_table_declaration **declaration) CA_USE_RESULT;

int
ca_schema_parse_script (struct ca_schema *schema, FILE *input);

/*****************************************************************************/

int
ca_table_write_time_float4 (struct ca_table *table, const char *key,
                            uint64_t start_time, uint32_t interval,
                            const float *sample_values, size_t sample_count) CA_USE_RESULT;

int
ca_table_write_table_declaration (struct ca_table *table,
                                  const char *table_name,
                                  const struct ca_table_declaration *decl) CA_USE_RESULT;

/*****************************************************************************/

uint64_t
ca_data_parse_integer (const uint8_t **input);

const char *
ca_data_parse_string (const uint8_t **input);

void
ca_data_parse_time_float4 (const uint8_t **input,
                           uint64_t *start_time, uint32_t *interval,
                           const float **sample_values, uint32_t *count);

/*****************************************************************************/

int
ca_table_sort (struct ca_table *output, struct ca_table *input) CA_USE_RESULT;

typedef int (*ca_merge_callback) (const char *key, const struct iovec *value,
                                  void *opaque);
int
ca_table_merge (struct ca_table **tables, size_t table_count,
                ca_merge_callback callback, void *opaque) CA_USE_RESULT;

/*****************************************************************************/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* !TABLE_H_ */
