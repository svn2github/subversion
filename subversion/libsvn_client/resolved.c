/*
 * resolved.c:  wrapper around wc resolved functionality.
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

/* ==================================================================== */



/*** Includes. ***/

#include "svn_types.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_hash.h"
#include "svn_sorts.h"
#include "client.h"
#include "private/svn_sorts_private.h"
#include "private/svn_wc_private.h"

#include "svn_private_config.h"

/*** Code. ***/

svn_error_t *
svn_client__resolve_conflicts(svn_boolean_t *conflicts_remain,
                              apr_hash_t *conflicted_paths,
                              svn_client_ctx_t *ctx,
                              apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_array_header_t *array;
  int i;

  if (conflicts_remain)
    *conflicts_remain = FALSE;

  SVN_ERR(svn_hash_keys(&array, conflicted_paths, scratch_pool));
  svn_sort__array(array, svn_sort_compare_paths);

  for (i = 0; i < array->nelts; i++)
    {
      const char *local_abspath = APR_ARRAY_IDX(array, i, const char *);

      svn_pool_clear(iterpool);
      SVN_ERR(svn_wc__resolve_conflicts(ctx->wc_ctx, local_abspath,
                                        svn_depth_empty,
                                        TRUE /* resolve_text */,
                                        "" /* resolve_prop (ALL props) */,
                                        TRUE /* resolve_tree */,
                                        svn_wc_conflict_choose_unspecified,
                                        ctx->conflict_func2,
                                        ctx->conflict_baton2,
                                        ctx->cancel_func, ctx->cancel_baton,
                                        ctx->notify_func2, ctx->notify_baton2,
                                        iterpool));

      if (conflicts_remain && !*conflicts_remain)
        {
          svn_error_t *err;
          svn_boolean_t text_c, prop_c, tree_c;

          err = svn_wc_conflicted_p3(&text_c, &prop_c, &tree_c,
                                     ctx->wc_ctx, local_abspath,
                                     iterpool);
          if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
            {
              svn_error_clear(err);
              text_c = prop_c = tree_c = FALSE;
            }
          else
            {
              SVN_ERR(err);
            }
          if (text_c || prop_c || tree_c)
            *conflicts_remain = TRUE;
        }
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_resolve(const char *path,
                   svn_depth_t depth,
                   svn_wc_conflict_choice_t conflict_choice,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  const char *local_abspath;
  svn_error_t *err;
  const char *lock_abspath;

  if (svn_path_is_url(path))
    return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                             _("'%s' is not a local path"), path);

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  /* Similar to SVN_WC__CALL_WITH_WRITE_LOCK but using a custom
     locking function. */

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath, pool, pool));
  err = svn_wc__resolve_conflicts(ctx->wc_ctx, local_abspath,
                                  depth,
                                  TRUE /* resolve_text */,
                                  "" /* resolve_prop (ALL props) */,
                                  TRUE /* resolve_tree */,
                                  conflict_choice,
                                  ctx->conflict_func2,
                                  ctx->conflict_baton2,
                                  ctx->cancel_func, ctx->cancel_baton,
                                  ctx->notify_func2, ctx->notify_baton2,
                                  pool);

  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 pool));
  svn_io_sleep_for_timestamps(path, pool);

  return svn_error_trace(err);
}


/*** Dealing with conflicts. ***/

svn_wc_conflict_kind_t
svn_client_conflict_get_kind(const svn_wc_conflict_description2_t *conflict)
{
  return conflict->kind;
}

const char *
svn_client_conflict_get_local_abspath(
  const svn_wc_conflict_description2_t *conflict)
{
  return conflict->local_abspath;
}

svn_wc_operation_t
svn_client_conflict_get_operation(
  const svn_wc_conflict_description2_t *conflict)
{
  return conflict->operation;
}

svn_wc_conflict_action_t
svn_client_conflict_get_incoming_change(
  const svn_wc_conflict_description2_t *conflict)
{
  return conflict->action;
}

svn_wc_conflict_reason_t
svn_client_conflict_get_local_change(
  const svn_wc_conflict_description2_t *conflict)
{
  return conflict->reason;
}

svn_node_kind_t
svn_client_conflict_tree_get_victim_node_kind(
  const svn_wc_conflict_description2_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(svn_client_conflict_get_kind(conflict)
      == svn_wc_conflict_kind_tree);

  return conflict->node_kind;
}

const char *
svn_client_conflict_prop_get_propname(
  const svn_wc_conflict_description2_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(svn_client_conflict_get_kind(conflict)
      == svn_wc_conflict_kind_property);

  return conflict->property_name;
}

static svn_error_t *
get_propval_from_filepath(const svn_string_t **propval,
                          const char *local_abspath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *buf;

  SVN_ERR(svn_stringbuf_from_file2(&buf, local_abspath, scratch_pool));
  *propval = svn_string_create_from_buf(buf, result_pool); 

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_prop_get_propvals(
  const svn_string_t **base_propval,
  const svn_string_t **my_propval,
  const svn_string_t **their_propval,
  const svn_wc_conflict_description2_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(svn_client_conflict_get_kind(conflict) ==
                 svn_wc_conflict_kind_property);

  /* ### Work around a historical bug in the provider: the path to the
   *     conflict description file was put in the 'theirs' field, and
   *     'theirs' was put in the 'merged' field. */
  ((svn_wc_conflict_description2_t *)conflict)->their_abspath =
    conflict->merged_file;
  ((svn_wc_conflict_description2_t *)conflict)->merged_file = NULL;

  if (base_propval)
    {
      if (conflict->base_abspath)
        SVN_ERR(get_propval_from_filepath(base_propval, conflict->base_abspath,
                                          result_pool, scratch_pool));
      else
        *base_propval = NULL;
    }

  if (my_propval)
    {
      if (conflict->my_abspath)
        SVN_ERR(get_propval_from_filepath(my_propval, conflict->my_abspath,
                                          result_pool, scratch_pool));
      else
        *my_propval = NULL;
    }

  if (their_propval)
    {
      if (conflict->their_abspath)
        SVN_ERR(get_propval_from_filepath(their_propval,
                                          conflict->their_abspath,
                                          result_pool, scratch_pool));
      else
        *their_propval = NULL;
    }

  return SVN_NO_ERROR;
}
