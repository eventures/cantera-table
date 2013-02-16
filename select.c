/*
    SQL SELECT query processor
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
#include <time.h>

#include "ca-table.h"
#include "query.h"

static const char *
expr_primary_key_filter (struct expression *expr, unsigned int primary_key_field)
{
  if (!expr)
    return NULL;

  switch (expr->type)
    {
    case EXPR_EQUAL:

      if (expr->lhs->type == EXPR_FIELD
          && expr->lhs->value.d.field_index == primary_key_field
          && expr->rhs->type == EXPR_CONSTANT
          && expr->rhs->value.type == CA_TEXT)
        {
          return expr->rhs->value.d.string_literal;
        }

      if (expr->rhs->type == EXPR_FIELD
          && expr->rhs->value.d.field_index == primary_key_field
          && expr->lhs->type == EXPR_CONSTANT
          && expr->lhs->value.type == CA_TEXT)
        {
          return expr->lhs->value.d.string_literal;
        }

      return NULL;

    case EXPR_AND:

        {
          const char *lhs, *rhs;

          lhs = expr_primary_key_filter (expr->lhs, primary_key_field);
          rhs = expr_primary_key_filter (expr->rhs, primary_key_field);

          if (lhs)
            {
              if (rhs && strcmp (lhs, rhs))
                return NULL;

              return lhs;
            }

          return rhs;
        }

    case EXPR_OR:

        {
          const char *lhs, *rhs;

          lhs = expr_primary_key_filter (expr->lhs, primary_key_field);
          rhs = expr_primary_key_filter (expr->rhs, primary_key_field);

          if (lhs && rhs && !strcmp (lhs, rhs))
            return lhs;

          return NULL;
        }

      break;

    default:

      return NULL;
    }
}

static int
CA_query_resolve_variables (struct expression *expression,
                            const struct ca_hashmap *variables,
                            int *is_constant)
{
  struct select_variable *variable;

  while (expression)
    {
      switch (expression->type)
        {
        case EXPR_IDENTIFIER:

          if (!(variable = ca_hashmap_get (variables, expression->value.d.identifier)))
            {
              ca_set_error ("Unknown field name '%s'", expression->value.d.identifier);

              return -1;
            }

          expression->type = EXPR_FIELD;
          expression->value.type = variable->type;
          expression->value.d.field_index = variable->field_index;

          if (is_constant)
            *is_constant = 0;

          break;

        case EXPR_ADD:
        case EXPR_AND:
        case EXPR_DIV:
        case EXPR_EQUAL:
        case EXPR_GREATER_EQUAL:
        case EXPR_GREATER_THAN:
        case EXPR_LESS_EQUAL:
        case EXPR_LESS_THAN:
        case EXPR_LIKE:
        case EXPR_MUL:
        case EXPR_NOT_EQUAL:
        case EXPR_NOT_LIKE:
        case EXPR_OR:
        case EXPR_SUB:

          if (-1 == CA_query_resolve_variables (expression->rhs, variables,
                                                is_constant))
            return -1;

          /* Fall through */

        case EXPR_CAST:
        case EXPR_DISTINCT:
        case EXPR_NEGATIVE:
        case EXPR_FUNCTION_CALL:

          if (-1 == CA_query_resolve_variables (expression->lhs, variables,
                                                is_constant))
            return -1;

          break;

        case EXPR_ASTERISK:
        case EXPR_CONSTANT:

          break;

        default:

          fprintf (stderr, "Type %d not implemented\n", expression->type);

          assert (!"fix me");
        }

      expression = expression->next;
    }

  return 0;
}

int
CA_select (struct ca_query_parse_context *context, struct select_statement *stmt)
{
  struct ca_schema *schema;
  struct ca_table *table;
  struct ca_table_declaration *declaration;

  struct iovec *field_values = NULL;

  struct select_variable *first_variable = NULL;
  struct select_variable **last_variable = &first_variable;
  struct ca_hashmap *variables = NULL;

  struct select_item *si;

  ca_expression_function output = NULL, where = NULL;
  ca_collect_function collect = NULL;

  size_t i;

  int ret, result = -1;

  unsigned int primary_key_index = 0;
  int has_unique_primary_key = -1;

  struct iovec value;

  struct ca_arena_info arena;

  ca_clear_error ();

  ca_arena_init (&arena);

  schema = context->schema;

  if (!(table = ca_schema_table (schema, stmt->from, &declaration)))
    goto done;

  collect = CA_collect_compile (declaration->fields, declaration->field_count);

  /*** Replace identifiers with pointers to the table field ***/

  variables = ca_hashmap_create (63);

  for (i = 0; i < declaration->field_count; ++i)
    {
      struct select_variable *new_variable;

      /* XXX: Use arena allocator */

      new_variable = calloc (1, sizeof (*new_variable));
      new_variable->name = declaration->fields[i].name;
      new_variable->field_index = i;
      new_variable->type = declaration->fields[i].type;

      if (declaration->fields[i].flags & CA_FIELD_PRIMARY_KEY)
        {
          if (has_unique_primary_key == -1)
            {
              has_unique_primary_key = 1;
              primary_key_index = i;
            }
          else
            has_unique_primary_key = 0;
        }

      if (-1 == ca_hashmap_insert (variables, declaration->fields[i].name, new_variable))
        goto done;

      *last_variable = new_variable;
      last_variable = &new_variable->next;
    }

  if (-1 == CA_query_resolve_variables (&stmt->list->expression, variables, NULL))
    goto done;

  /*** Resolve names of columns ***/

  for (si = stmt->list; si; si = (struct select_item *) si->expression.next)
    {
      if (si->alias)
        continue;

      if (si->expression.type == EXPR_FIELD)
        si->alias = declaration->fields[si->expression.value.d.field_index].name;
      else
        si->alias = "?column?";
    }

  if (!(output = CA_expression_compile ("output",
                                        &stmt->list->expression,
                                        declaration->fields,
                                        declaration->field_count,
                                        CA_EXPRESSION_PRINT)))
    {
      goto done;
    }

  if (stmt->where)
    {
      int is_constant = 1;

      if (-1 == CA_query_resolve_variables (stmt->where, variables, &is_constant))
        goto done;

      if (!(where = CA_expression_compile ("where",
                                           stmt->where,
                                           declaration->fields,
                                           declaration->field_count,
                                           CA_EXPRESSION_RETURN_BOOL)))
        {
          ca_set_error ("Failed to compile WHERE expression: %s", ca_last_error ());

          goto done;
        }

      if (is_constant)
        {
          if (!where (context, NULL))
            stmt->limit = 0;

          where = NULL;
        }
    }

  if (!stmt->limit)
    {
      if (CA_output_format == CA_PARAM_VALUE_JSON)
        printf ("[]\n");

      result = 0;

      goto done;
    }

  if (!(field_values = ca_malloc (sizeof (*field_values) * declaration->field_count)))
    goto done;

  const char *primary_key_filter;

  if (CA_output_format == CA_PARAM_VALUE_JSON)
    putchar ('[');

  if (1 == has_unique_primary_key
      && NULL != (primary_key_filter = expr_primary_key_filter (stmt->where, primary_key_index)))
    {
      if (-1 == (ret = ca_table_seek_to_key (table, primary_key_filter)))
        goto done;

      if (!ret)
        return 0; /* Key does not exist in table */

      if (0 >= (ret = ca_table_read_row (table, &value)))
        {
          if (!ret)
            ca_set_error ("ca_table_read_row on '%s' unexpectedly returned %d",
                          stmt->from, (int) ret);

          goto done;
        }

      collect (field_values, value.iov_base,
               (const uint8_t *) value.iov_base + value.iov_len);

      if (!where || where (context, field_values))
        {
          if (-1 == output (context, field_values))
            goto done;
        }
    }
  else
    {
      int first = 1;

      if (-1 == (ret = ca_table_seek (table, 0, SEEK_SET)))
        goto done;

      while (0 < (ret = ca_table_read_row (table, &value)))
        {
          collect (field_values, value.iov_base,
                   (const uint8_t *) value.iov_base + value.iov_len);

          if (where && !where (context, field_values))
            continue;

          if (stmt->offset > 0 && stmt->offset--)
            continue;

          if (!first && CA_output_format == CA_PARAM_VALUE_JSON)
            putchar (',');

          if (-1 == output (context, field_values))
            goto done;

          if (stmt->limit > 0 && !--stmt->limit)
            break;

          ca_arena_reset (&arena);

          first = 0;
        }

      if (ret == -1)
        goto done;
    }

  if (CA_output_format == CA_PARAM_VALUE_JSON)
    printf ("]\n");

  result = 0;

done:

  ca_arena_free (&arena);

  free (field_values);
  ca_hashmap_free (variables);

  return result;
}
