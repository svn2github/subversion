/*
 * wc-queries-test.c -- test the evaluation of the wc Sqlite queries
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_pools.h"
#include "svn_ctype.h"

#include "svn_private_config.h"

#include "../svn_test.h"

#ifdef SVN_SQLITE_INLINE
/* Include sqlite3 inline, making all symbols private. */
  #define SQLITE_API static
  #include <sqlite3.c>
#else
  #include <sqlite3.h>
#endif

#include "../../libsvn_wc/wc-queries.h"

WC_QUERIES_SQL_DECLARE_STATEMENTS(wc_queries);

/* The first query after the normal wc queries */
#define STMT_SCHEMA_FIRST STMT_CREATE_SCHEMA

#define SQLITE_ERR(x)   \
{                                                                \
  int sqlite_err__temp = (x);                                    \
  if (sqlite_err__temp != SQLITE_OK)                             \
    return svn_error_createf(SVN_ERR_SQLITE_ERROR,               \
                             NULL, "sqlite: %s",                 \
                             sqlite3_errmsg(sdb));               \
} while (0)

/* Schema creation statements fail during preparing when the table
   already exists, and must be evaluated before testing the
   queries. Statements above STMT_SCHEMA_FIRST only need to be
   included here when they need to be evaluated before testing the
   statements */
static const int schema_statements[] =
{
  /* Usual tables */
  STMT_CREATE_SCHEMA,
  STMT_CREATE_NODES,
  STMT_CREATE_NODES_TRIGGERS,
  STMT_CREATE_EXTERNALS,
  /* Memory tables */
  STMT_CREATE_TARGETS_LIST,
  STMT_CREATE_CHANGELIST_LIST,
  STMT_CREATE_NODE_PROPS_CACHE,
  STMT_CREATE_REVERT_LIST,
  STMT_CREATE_DELETE_LIST,
  -1 /* final marker */
};

/* These statements currently trigger warnings. It would be nice if
   we could annotate these in wc-queries.sql */
static const int slow_statements[] =
{
  STMT_CLEAR_BASE_NODE_RECURSIVE_DAV_CACHE,
  STMT_RECURSIVE_UPDATE_NODE_REPO,
  -1 /* final marker */
};

/* Helper function to determine if a statement is in a list */
static svn_boolean_t
in_list(const int list[], int stmt_idx)
{
  int i;

  for (i = 0; list[i] != -1; i++)
    {
      if (list[i] == stmt_idx)
        return TRUE;
    }
  return FALSE;
}

/* Helpers to determine if a statement is in a common list */
#define is_slow_statement(stmt_idx) in_list(slow_statements, stmt_idx)
#define is_schema_statement(stmt_idx) \
    ((stmt_idx >= STMT_SCHEMA_FIRST) || in_list(schema_statements, stmt_idx))


/* Create an in-memory db for evaluating queries */
static svn_error_t *
create_memory_db(sqlite3 **db,
                 apr_pool_t *pool)
{
  sqlite3 *sdb;
  int i;

  /* Create an in-memory raw database */
  SVN_TEST_ASSERT(sqlite3_initialize() == SQLITE_OK);
  SQLITE_ERR(sqlite3_open_v2("", &sdb,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL));

  /* Create schema */
  for (i = 0; schema_statements[i] != -1; i++)
    {
      SQLITE_ERR(sqlite3_exec(sdb, wc_queries[schema_statements[i]], NULL, NULL, NULL));
    }

  *db = sdb;
  return SVN_NO_ERROR;
}

/* Parse all normal queries */
static svn_error_t *
test_parsable(apr_pool_t *scratch_pool)
{
  sqlite3 *sdb;
  int i;

  SVN_ERR(create_memory_db(&sdb, scratch_pool));

  for (i=0; i < STMT_SCHEMA_FIRST; i++)
    {
      sqlite3_stmt *stmt;
      const char *text = wc_queries[i];

      if (is_schema_statement(i))
        continue;

      /* Some of our statement texts contain multiple queries. We prepare
         them all. */
      while (*text != '\0')
        {
          const char *tail;
          int r = sqlite3_prepare_v2(sdb, text, -1, &stmt, &tail);

          if (r != SQLITE_OK)
            return svn_error_createf(SVN_ERR_SQLITE_ERROR, NULL,
                                     "Preparing statement %d failed: %s\n%s",
                                     i, sqlite3_errmsg(sdb),
                                     text);

          SQLITE_ERR(sqlite3_finalize(stmt));

          /* Continue after the current statement */
          text = tail;
        }
    }

  SQLITE_ERR(sqlite3_close(sdb)); /* Close the DB if ok; otherwise leaked */

  return SVN_NO_ERROR;
}

/* Contains a parsed record from EXPLAIN QUERY PLAN */
struct explanation_item
{
  const char *operation;
  const char *table;
  const char *alias;
  svn_boolean_t scan;
  svn_boolean_t covered_by_index;
  svn_boolean_t primarary_key;
  svn_boolean_t automatic_index;
  const char *index;
  const char *expressions;
  const char *expected;
  int expression_vars;
  int expected_rows;
};

#define MATCH_TOKEN(x, y) (x && (strcmp(x, y) == 0))

/* Simple parser for the Sqlite textual explanation into an explanation_item.
   Writes "DBG:" lines when sqlite produces unexpected results. When no
   valid explanation_item can be parsed sets *PARSED_ITEM to NULL, otherwise
   to a valid result. */
static svn_error_t *
parse_explanation_item(struct explanation_item **parsed_item,
                       const char *text,
                       apr_pool_t *result_pool)
{
  struct explanation_item *item = apr_pcalloc(result_pool, sizeof(*item));
  char *token;
  char *last;
  char *tmp = apr_pstrdup(result_pool, text);
  const char *tmp_end = &tmp[strlen(tmp)];

  *parsed_item = NULL;

  item->operation = apr_strtok(tmp, " ", &last);

  if (!item->operation)
    {
      return SVN_NO_ERROR;
    }

  item->scan = MATCH_TOKEN(item->operation, "SCAN");

  if (item->scan || MATCH_TOKEN(item->operation, "SEARCH"))
    {
      token = apr_strtok(NULL, " ", &last);

      if (!MATCH_TOKEN(token, "TABLE"))
        {
          printf("DBG: Expected 'TABLE', got '%s' in '%s'\n", token, text);
          return SVN_NO_ERROR; /* Nothing to parse */
        }

      item->table = apr_strtok(NULL, " ", &last);

      token = apr_strtok(NULL, " ", &last);

      /* Skip alias */
      if (MATCH_TOKEN(token, "AS"))
        {
          item->alias = apr_strtok(NULL, " ", &last);
          token = apr_strtok(NULL, " ", &last);
        }

      if (MATCH_TOKEN(token, "USING"))
        {
          token = apr_strtok(NULL, " ", &last);

          if (MATCH_TOKEN(token, "AUTOMATIC"))
            {
              /* Pain: A temporary index is created */
              item->automatic_index = TRUE;
              token = apr_strtok(NULL, " ", &last);
            }

          /* Handle COVERING */
          if (MATCH_TOKEN(token, "COVERING"))
            {
              /* Bonus: Query will be answered by just using the index */
              item->covered_by_index = TRUE;
              token = apr_strtok(NULL, " ", &last);
           }

          if (MATCH_TOKEN(token, "INDEX"))
            {
              item->index = apr_strtok(NULL, " ", &last);
            }
          else if (MATCH_TOKEN(token, "INTEGER"))
            {
              token = apr_strtok(NULL, " ", &last);
              if (!MATCH_TOKEN(token, "PRIMARY"))
                {
                  printf("DBG: Expected 'PRIMARY', got '%s' in '%s'\n",
                         token, text);
                  return SVN_NO_ERROR;
                }

              token = apr_strtok(NULL, " ", &last);
              if (!MATCH_TOKEN(token, "KEY"))
                {
                  printf("DBG: Expected 'KEY', got '%s' in '%s'\n",
                         token, text);
                  return SVN_NO_ERROR;
                }

              item->primarary_key = TRUE;
            }
          else
            {
              printf("DBG: Expected 'INDEX' or 'PRIMARY', got '%s' in '%s'\n",
                     token, text);
              return SVN_NO_ERROR;
            }

          token = apr_strtok(NULL, " ", &last);
        }

      if (token && token[0] == '(' && token[1] != '~')
        {
          /* Undo the tokenization to switch parser rules */
          size_t token_len = strlen(token);

          if (token + token_len < tmp_end)
            token[token_len] = ' ';

          if (token[token_len] == '\0')
            last[-1] = ' ';

          token++; /* Skip the '(' */

          item->expressions = apr_strtok(token, ")", &last);
          token = apr_strtok(NULL, " ", &last);
        }

      if (token && *token == '(' && token[1] == '~')
        {
          /* Undo the tokenization to switch parser rules */
          size_t token_len = strlen(token);

          if (token + token_len < tmp_end)
            token[token_len] = ' ';

          if (token[token_len] == '\0')
            last[-1] = ' ';

          token += 2; /* Skip "(~" */

          item->expected = apr_strtok(token, ")", &last);
          token = apr_strtok(NULL, " ", &last);
        }

      if (token)
        {
          printf("DBG: Unexpected token '%s' in '%s'\n",
                 token, text);
          return SVN_NO_ERROR;
        }

      /* Parsing successfull */
    }
  else if (MATCH_TOKEN(item->operation, "EXECUTE"))
    {
      /* Subquery handling */
      return SVN_NO_ERROR;
    }
  else if (MATCH_TOKEN(item->operation, "COMPOUND"))
    {
      /* Handling temporary table (E.g. UNION) */
      return SVN_NO_ERROR;
    }
  else
    {
      printf("DBG: Unhandled sqlite operation '%s' in explanation\n", item->operation);
      return SVN_NO_ERROR;
    }

  if (item->expressions)
    {
      const char *p;

      for (p = item->expressions; *p; p++)
        {
          if (*p == '?')
            item->expression_vars++;
        }
    }
  if (item->expected)
    {
      item->expected_rows = atoi(item->expected);
    }

  *parsed_item = item;
  return SVN_NO_ERROR;
}

/* Returns TRUE if TABLE_NAME specifies a nodes table, which should be indexed
   by wc_id and either local_relpath or parent_relpath */
static svn_boolean_t
is_node_table(const char *table_name)
{
  return (apr_strnatcasecmp(table_name, "nodes") == 0
          || apr_strnatcasecmp(table_name, "actual") == 0
          || apr_strnatcasecmp(table_name, "externals") == 0);
}

static svn_error_t *
test_query_expectations(apr_pool_t *scratch_pool)
{
  sqlite3 *sdb;
  int i;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  svn_error_t *warnings = NULL;

  SVN_ERR(create_memory_db(&sdb, scratch_pool));

  /* Sqlite has an SQLITE_OMIT_EXPLAIN compilation flag. In this case the
     'EXPLAIN QUERY PLAN' option is currently just ignored and the query
     evaluated (status at Sqlite 3.7.12).
     
     Detect this case, and skip this test */
  {
    sqlite3_stmt *stmt;
    int r;
    r = sqlite3_prepare(sdb, "EXPLAIN QUERY PLAN SELECT 101010101",
                        -1, &stmt, NULL);

    if (r != SQLITE_OK)
      {
        SQLITE_ERR(sqlite3_close(sdb));
        return svn_error_create(SVN_ERR_TEST_SKIPPED, warnings,
                                "Sqlite doesn't support EXPLAIN QUERY PLAN");
      }

    if (sqlite3_step(stmt) == SQLITE_ROW)
      {
        if (sqlite3_column_count(stmt) == 1
            && sqlite3_column_int(stmt, 0) == 101010101)
          {
            SQLITE_ERR(sqlite3_reset(stmt));
            SQLITE_ERR(sqlite3_finalize(stmt));
            SQLITE_ERR(sqlite3_close(sdb));
            return svn_error_create(SVN_ERR_TEST_SKIPPED, warnings,
                                "Sqlite doesn't support EXPLAIN QUERY PLAN");
          }
      }
    SQLITE_ERR(sqlite3_reset(stmt));
    SQLITE_ERR(sqlite3_finalize(stmt));
  }

  for (i=0; i < STMT_SCHEMA_FIRST; i++)
    {
      sqlite3_stmt *stmt;
      const char *tail;
      int r;

      if (is_schema_statement(i))
        continue;

      /* Prepare statement to find if it is a single statement. */
      r = sqlite3_prepare_v2(sdb, wc_queries[i], -1, &stmt, &tail);

      if (r != SQLITE_OK)
        break; /* Parse failure is already reported by 'test_parable' */

      SQLITE_ERR(sqlite3_finalize(stmt));
      if (tail[0] != '\0')
        break; /* Multi-queries are currently not testable */

      svn_pool_clear(iterpool);

      r = sqlite3_prepare_v2(sdb,
                             apr_pstrcat(iterpool,
                                         "EXPLAIN QUERY PLAN ",
                                         wc_queries[i],
                                         NULL),
                             -1, &stmt, &tail);

      if (r != SQLITE_OK)
        continue; /* EXPLAIN not enabled or doesn't support this query */

      while (SQLITE_ROW == (r = sqlite3_step(stmt)))
        {
          /*int iSelectid;
          int iOrder;
          int iFrom;*/
          const unsigned char *zDetail;
          char *detail;
          struct explanation_item *item;

          /* ### The following code is correct for current Sqlite versions
             ### (tested with 3.6.x, 3.7.x), but the EXPLAIN QUERY PLAN output
             ### is not guaranteed to be stable for future versions. */

          if (sqlite3_column_count(stmt) < 4)
            {
              return svn_error_create(
                                SVN_ERR_TEST_SKIPPED, NULL,
                                "EXPLAIN QUERY PLAN doesn't return 4 columns");
            }

          /* Names as in Sqlite documentation */
          /*iSelectid = sqlite3_column_int(stmt, 0);
          iOrder = sqlite3_column_int(stmt, 1);
          iFrom = sqlite3_column_int(stmt, 2);*/
          zDetail = sqlite3_column_text(stmt, 3);

          detail = apr_pstrdup(iterpool, (const char*)zDetail);

          SVN_ERR(parse_explanation_item(&item, detail, iterpool));

          if (!item)
            continue; /* Not parsable or not interesting */

          if (item->automatic_index)
            {
              warnings = svn_error_createf(SVN_ERR_TEST_FAILED, warnings,
                                "WC-Query %d: "
                                "Creates a temporary index: %s\n",
                                i, wc_queries[i]);
              break;
            }
          else if (((item->expression_vars < 2 && is_node_table(item->table))
                   || (item->expression_vars < 1)) && !is_slow_statement(i))
            {
              warnings = svn_error_createf(SVN_ERR_TEST_FAILED, warnings,
                                "WC-Query %d: "
                                "Uses %s with only %d index component: (%s)\n%s",
                                i, item->table, item->expression_vars,
                                item->expressions, wc_queries[i]);
              break;
            }
          else if (!item->primarary_key && !item->index)
            {
              warnings = svn_error_createf(SVN_ERR_TEST_FAILED, warnings,
                                "WC-Query %d: "
                                "Query on %s doesn't use an index:\n%s",
                                i, item->table, wc_queries[i]);
              break;
            }
          else if (item->scan)
            {
              warnings = svn_error_createf(SVN_ERR_TEST_FAILED, warnings,
                                "WC-Query %d: "
                                "Performs scan on %s:\n%s",
                                i, item->table, wc_queries[i]);
              break;
            }
        }
      SQLITE_ERR(sqlite3_reset(stmt));
      SQLITE_ERR(sqlite3_finalize(stmt));
    }
  SQLITE_ERR(sqlite3_close(sdb)); /* Close the DB if ok; otherwise leaked */

  return warnings;
}

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_parsable,
                   "queries are parsable"),
    SVN_TEST_PASS2(test_query_expectations,
                   "test query expectations"),
    SVN_TEST_NULL
  };
