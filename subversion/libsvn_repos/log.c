/* log.c --- retrieving log messages
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */


#include <stdlib.h>
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_private_config.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_string.h"
#include "svn_sorts.h"
#include "svn_props.h"
#include "svn_mergeinfo.h"
#include "repos.h"

/* Pass history information about REV to RECEIVER with its RECEIVER_BATON.
 *
 * FS is used with REV to fetch the interesting history information,
 * such as author, date, etc.
 *
 * The detect_changed function is used if either AUTHZ_READ_FUNC is
 * not NULL, or if DISCOVER_CHANGED_PATHS is TRUE.  See it for details.
 *
 * If DESCENDING_ORDER is true, send child messages in descending order.
 * 
 * If OMIT_LOG_TEXT is true, don't send the log text to RECEIVER.
 *
 * If INCLUDE_MERGED_REVISIONS is TRUE, also pass history information to
 * RECEIVER for any revisions which were merged in a result of REV.
 */
static svn_error_t *
send_change_rev(const apr_array_header_t *paths,
                svn_revnum_t rev,
                svn_fs_t *fs,
                svn_boolean_t discover_changed_paths,
                svn_boolean_t include_merged_revisions,
                svn_boolean_t omit_log_text,
                svn_boolean_t descending_order,
                svn_repos_authz_func_t authz_read_func,
                void *authz_read_baton,
                svn_log_message_receiver2_t receiver,
                void *receiver_baton,
                apr_pool_t *pool);


svn_error_t *
svn_repos_check_revision_access(svn_repos_revision_access_level_t *access,
                                svn_repos_t *repos,
                                svn_revnum_t revision,
                                svn_repos_authz_func_t authz_read_func,
                                void *authz_read_baton,
                                apr_pool_t *pool)
{
  svn_fs_t *fs = svn_repos_fs(repos);
  svn_fs_root_t *rev_root;
  apr_hash_t *changes;
  apr_hash_index_t *hi;
  svn_boolean_t found_readable = FALSE;
  svn_boolean_t found_unreadable = FALSE;
  apr_pool_t *subpool;

  /* By default, we'll grant full read access to REVISION. */
  *access = svn_repos_revision_access_full;

  /* No auth-checking function?  We're done. */
  if (! authz_read_func)
    return SVN_NO_ERROR;

  /* Fetch the changes associated with REVISION. */
  SVN_ERR(svn_fs_revision_root(&rev_root, fs, revision, pool));
  SVN_ERR(svn_fs_paths_changed(&changes, rev_root, pool));

  /* No changed paths?  We're done. */
  if (apr_hash_count(changes) == 0)
    return SVN_NO_ERROR;

  /* Otherwise, we have to check the readability of each changed
     path, or at least enough to answer the question asked. */
  subpool = svn_pool_create(pool);
  for (hi = apr_hash_first(NULL, changes); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      svn_fs_path_change_t *change;
      svn_boolean_t readable;

      svn_pool_clear(subpool);
      apr_hash_this(hi, &key, NULL, &val);
      change = val;

      SVN_ERR(authz_read_func(&readable, rev_root, key, 
                              authz_read_baton, subpool));
      if (! readable)
        found_unreadable = TRUE;
      else
        found_readable = TRUE;

      /* If we have at least one of each (readable/unreadable), we
         have our answer. */
      if (found_readable && found_unreadable)
        goto decision;

      switch (change->change_kind)
        {
        case svn_fs_path_change_add:
        case svn_fs_path_change_replace:
          {
            const char *copyfrom_path;
            svn_revnum_t copyfrom_rev;

            SVN_ERR(svn_fs_copied_from(&copyfrom_rev, &copyfrom_path,
                                       rev_root, key, subpool));
            if (copyfrom_path && SVN_IS_VALID_REVNUM(copyfrom_rev))
              {
                svn_fs_root_t *copyfrom_root;
                SVN_ERR(svn_fs_revision_root(&copyfrom_root, fs,
                                             copyfrom_rev, subpool));
                SVN_ERR(authz_read_func(&readable,
                                        copyfrom_root, copyfrom_path,
                                        authz_read_baton, subpool));
                if (! readable)
                  found_unreadable = TRUE;

                /* If we have at least one of each (readable/unreadable), we
                   have our answer. */
                if (found_readable && found_unreadable)
                  goto decision;
              }
          }
          break;

        case svn_fs_path_change_delete:
        case svn_fs_path_change_modify:
        default:
          break;
        }
    }

 decision:
  svn_pool_destroy(subpool);

  /* Either every changed path was unreadable... */
  if (! found_readable)
    *access = svn_repos_revision_access_none;

  /* ... or some changed path was unreadable... */
  else if (found_unreadable)
    *access = svn_repos_revision_access_partial;

  /* ... or every changed path was readable (the default). */
  return SVN_NO_ERROR;
}


/* Store as keys in CHANGED the paths of all node in ROOT that show a
 * significant change.  "Significant" means that the text or
 * properties of the node were changed, or that the node was added or
 * deleted.
 *
 * The CHANGED hash set and its keys and values are allocated in POOL;
 * keys are const char * paths and values are svn_log_changed_path_t.
 * 
 * If optional AUTHZ_READ_FUNC is non-NULL, then use it (with
 * AUTHZ_READ_BATON and FS) to check whether each changed-path (and
 * copyfrom_path) is readable:
 *
 *     - If some paths are readable and some are not, then silently
 *     omit the unreadable paths from the CHANGED hash, and return
 *     SVN_ERR_AUTHZ_PARTIALLY_READABLE.
 *
 *     - If absolutely every changed-path (and copyfrom_path) is
 *     unreadable, then return an empty CHANGED hash and
 *     SVN_ERR_AUTHZ_UNREADABLE.  (This is to distinguish a revision
 *     which truly has no changed paths from a revision in which all
 *     paths are unreadable.)
 */
static svn_error_t *
detect_changed(apr_hash_t **changed,
               svn_fs_root_t *root,
               svn_fs_t *fs,
               svn_repos_authz_func_t authz_read_func,
               void *authz_read_baton,
               apr_pool_t *pool)
{
  apr_hash_t *changes;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_boolean_t found_readable = FALSE;
  svn_boolean_t found_unreadable = FALSE;

  *changed = apr_hash_make(pool);
  SVN_ERR(svn_fs_paths_changed(&changes, root, pool));

  if (apr_hash_count(changes) == 0)
    /* No paths changed in this revision?  Uh, sure, I guess the
       revision is readable, then.  */
    return SVN_NO_ERROR;

  for (hi = apr_hash_first(pool, changes); hi; hi = apr_hash_next(hi))
    {
      /* NOTE:  Much of this loop is going to look quite similar to
         svn_repos_check_revision_access(), but we have to do more things
         here, so we'll live with the duplication. */
      const void *key;
      void *val;
      svn_fs_path_change_t *change;
      const char *path;
      char action;
      svn_log_changed_path_t *item;

      svn_pool_clear(subpool);

      /* KEY will be the path, VAL the change. */
      apr_hash_this(hi, &key, NULL, &val);
      path = (const char *) key;
      change = val;

      /* Skip path if unreadable. */
      if (authz_read_func)
        {
          svn_boolean_t readable;
          SVN_ERR(authz_read_func(&readable,
                                  root, path,
                                  authz_read_baton, subpool));
          if (! readable)
            {
              found_unreadable = TRUE;
              continue;
            }
        }

      /* At least one changed-path was readable. */
      found_readable = TRUE;

      switch (change->change_kind)
        {
        case svn_fs_path_change_reset:
          continue;

        case svn_fs_path_change_add:
          action = 'A';
          break;

        case svn_fs_path_change_replace:
          action = 'R';
          break;

        case svn_fs_path_change_delete:
          action = 'D';
          break;

        case svn_fs_path_change_modify:
        default:
          action = 'M';
          break;
        }

      item = apr_pcalloc(pool, sizeof(*item));
      item->action = action;
      item->copyfrom_rev = SVN_INVALID_REVNUM;
      if ((action == 'A') || (action == 'R'))
        {
          const char *copyfrom_path;
          svn_revnum_t copyfrom_rev;

          SVN_ERR(svn_fs_copied_from(&copyfrom_rev, &copyfrom_path,
                                     root, path, subpool));

          if (copyfrom_path && SVN_IS_VALID_REVNUM(copyfrom_rev))
            {
              svn_boolean_t readable = TRUE;

              if (authz_read_func)
                {
                  svn_fs_root_t *copyfrom_root;
                  
                  SVN_ERR(svn_fs_revision_root(&copyfrom_root, fs,
                                               copyfrom_rev, subpool));
                  SVN_ERR(authz_read_func(&readable,
                                          copyfrom_root, copyfrom_path,
                                          authz_read_baton, subpool));
                  if (! readable)
                    found_unreadable = TRUE;
                }

              if (readable)
                {
                  item->copyfrom_path = apr_pstrdup(pool, copyfrom_path);
                  item->copyfrom_rev = copyfrom_rev;
                }
            }
        }
      apr_hash_set(*changed, apr_pstrdup(pool, path), 
                   APR_HASH_KEY_STRING, item);
    }

  svn_pool_destroy(subpool);

  if (! found_readable)
    /* Every changed-path was unreadable. */
    return svn_error_create(SVN_ERR_AUTHZ_UNREADABLE,
                            NULL, NULL);

  if (found_unreadable)
    /* At least one changed-path was unreadable. */
    return svn_error_create(SVN_ERR_AUTHZ_PARTIALLY_READABLE,
                            NULL, NULL);

  /* Every changed-path was readable. */
  return SVN_NO_ERROR;
}

/* This is used by svn_repos_get_logs3 to keep track of multiple
 * path history information while working through history.
 *
 * The two pools are swapped after each iteration through history because
 * to get the next history requires the previous one.
 */
struct path_info
{
  svn_stringbuf_t *path;
  svn_revnum_t history_rev;
  svn_boolean_t done;
  svn_boolean_t first_time;

  /* If possible, we like to keep open the history object for each path,
     since it avoids needed to open and close it many times as we walk
     backwards in time.  To do so we need two pools, so that we can clear
     one each time through.  If we're not holding the history open for
     this path then these three pointers will be NULL. */
  svn_fs_history_t *hist;
  apr_pool_t *newpool;
  apr_pool_t *oldpool;
};

/* Advance to the next history for the path.
 *
 * If INFO->HIST is not NULL we do this using that existing history object,
 * otherwise we open a new one.
 *
 * If no more history is available or the history revision is less
 * than (earlier) than START, or the history is not available due
 * to authorization, then INFO->DONE is set to TRUE.
 *
 * A STRICT value of FALSE will indicate to follow history across copied
 * paths.
 *
 * If optional AUTHZ_READ_FUNC is non-NULL, then use it (with
 * AUTHZ_READ_BATON and FS) to check whether INFO->PATH is still readable if
 * we do indeed find more history for the path.
 */
static svn_error_t *
get_history(struct path_info *info,
            svn_fs_t *fs,
            svn_boolean_t strict,
            svn_repos_authz_func_t authz_read_func,
            void *authz_read_baton,
            svn_revnum_t start,
            apr_pool_t *pool)
{
  svn_fs_root_t *history_root = NULL;
  svn_fs_history_t *hist;
  apr_pool_t *subpool;
  const char *path;

  if (info->hist)
    {
      subpool = info->newpool;

      SVN_ERR(svn_fs_history_prev(&info->hist, info->hist,
                                  strict ? FALSE : TRUE, subpool));

      hist = info->hist;
    }
  else
    {
      subpool = svn_pool_create(pool);

      /* Open the history located at the last rev we were at. */
      SVN_ERR(svn_fs_revision_root(&history_root, fs, info->history_rev,
                                   subpool));

      SVN_ERR(svn_fs_node_history(&hist, history_root, info->path->data,
                                  subpool));

      SVN_ERR(svn_fs_history_prev(&hist, hist, strict ? FALSE : TRUE,
                                  subpool));

      if (info->first_time)
        info->first_time = FALSE;
      else
        SVN_ERR(svn_fs_history_prev(&hist, hist, strict ? FALSE : TRUE,
                                    subpool));
    }

  if (! hist)
    {
      svn_pool_destroy(subpool);
      if (info->oldpool)
        svn_pool_destroy(info->oldpool);
      info->done = TRUE;
      return SVN_NO_ERROR;
    }

  /* Fetch the location information for this history step. */
  SVN_ERR(svn_fs_history_location(&path, &info->history_rev,
                                  hist, subpool));

  svn_stringbuf_set(info->path, path);

  /* If this history item predates our START revision then
     don't fetch any more for this path. */
  if (info->history_rev < start)
    {
      svn_pool_destroy(subpool);
      if (info->oldpool)
        svn_pool_destroy(info->oldpool);
      info->done = TRUE;
      return SVN_NO_ERROR;
    }
  
  /* Is the history item readable?  If not, done with path. */
  if (authz_read_func)
    {
      svn_boolean_t readable;
      SVN_ERR(svn_fs_revision_root(&history_root, fs,
                                   info->history_rev,
                                   subpool));
      SVN_ERR(authz_read_func(&readable, history_root,
                              info->path->data,
                              authz_read_baton,
                              subpool));
      if (! readable)
        info->done = TRUE;
    }

  if (! info->hist)
    {
      svn_pool_destroy(subpool);
    }
  else
    {
      apr_pool_t *temppool = info->oldpool;
      info->oldpool = info->newpool;
      svn_pool_clear(temppool);
      info->newpool = temppool;
    }

  return SVN_NO_ERROR;
}

/* Set INFO->HIST to the next history for the path *if* there is history
 * available and INFO->HISTORY_REV is equal to or greater than CURRENT.
 *
 * *CHANGED is set to TRUE if the path has history in the CURRENT revision,
 * otherwise it is not touched.
 *
 * If we do need to get the next history revision for the path, call
 * get_history to do it -- see it for details.
 */
static svn_error_t *
check_history(svn_boolean_t *changed,
              struct path_info *info,
              svn_fs_t *fs,
              svn_revnum_t current,
              svn_boolean_t strict,
              svn_repos_authz_func_t authz_read_func,
              void *authz_read_baton,
              svn_revnum_t start,
              apr_pool_t *pool)
{
  /* If we're already done with histories for this path,
     don't try to fetch any more. */
  if (info->done)
    return SVN_NO_ERROR;

  /* If the last rev we got for this path is less than CURRENT,
     then just return and don't fetch history for this path.
     The caller will get to this rev eventually or else reach
     the limit. */
  if (info->history_rev < current)
    return SVN_NO_ERROR;

  /* If the last rev we got for this path is equal to CURRENT
     then set *CHANGED to true and get the next history
     rev where this path was changed. */
  *changed = TRUE;
  SVN_ERR(get_history(info, fs, strict, authz_read_func,
                      authz_read_baton, start, pool));
  return SVN_NO_ERROR;
}

/* Return the next interesting revision in our list of HISTORIES. */
static svn_revnum_t
next_history_rev(apr_array_header_t *histories)
{
  svn_revnum_t next_rev = SVN_INVALID_REVNUM;
  int i;

  for (i = 0; i < histories->nelts; ++i)
    {
      struct path_info *info = APR_ARRAY_IDX(histories, i,
                                             struct path_info *);
      if (info->done)
        continue;
      if (info->history_rev > next_rev)
        next_rev = info->history_rev;
    }

  return next_rev;
}

/* Return the combined rangelists for everyone's mergeinfo for the
   PATHS tree at REV in *RANGELIST.  Perform all allocations in POOL. */
static svn_error_t *
get_combined_mergeinfo(apr_hash_t **mergeinfo,
                       svn_fs_t *fs,
                       svn_revnum_t rev,
                       const apr_array_header_t *paths,
                       apr_pool_t *pool)
{
  svn_fs_root_t *root;
  apr_hash_index_t *hi;
  apr_hash_t *tree_mergeinfo;
  apr_pool_t *subpool = svn_pool_create(pool);
  
  /* Get the mergeinfo for each tree roots in PATHS. */
  SVN_ERR(svn_fs_revision_root(&root, fs, rev, subpool));
  SVN_ERR(svn_fs_get_mergeinfo_for_tree(&tree_mergeinfo, root, paths, pool));

  *mergeinfo = apr_hash_make(pool);

  /* Merge all the mergeinfos into one mergeinfo */
  for (hi = apr_hash_first(subpool, tree_mergeinfo); hi; hi = apr_hash_next(hi))
    {
      apr_hash_t *path_mergeinfo;

      apr_hash_this(hi, NULL, NULL, (void *)&path_mergeinfo);
      SVN_ERR(svn_mergeinfo_merge(mergeinfo, path_mergeinfo, pool));
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Combine and return in *RANGELIST the various rangelists for each bit of
   MERGEINFO.  */
static svn_error_t *
combine_mergeinfo_rangelists(apr_array_header_t **rangelist,
                             apr_hash_t *mergeinfo,
                             apr_pool_t *pool)
{
  apr_hash_index_t *hi;

  *rangelist = apr_array_make(pool, 0, sizeof(svn_merge_range_t *));

  /* Iterate over each path's rangelist, and merge them into RANGELIST. */
  for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
    {
      apr_array_header_t *path_rangelist;

      apr_hash_this(hi, NULL, NULL, (void *)&path_rangelist);
      SVN_ERR(svn_rangelist_merge(rangelist, path_rangelist, pool));
    }

  return SVN_NO_ERROR;
}

/* Determine all the revisions which were merged into PATHS in REV.  Return
   them as a new RANGELIST.  */
static svn_error_t *
get_merged_rev_mergeinfo(apr_hash_t **mergeinfo,
                         svn_fs_t *fs,
                         const apr_array_header_t *paths,
                         svn_revnum_t rev,
                         apr_pool_t *pool)
{
  apr_hash_t *curr_mergeinfo, *prev_mergeinfo;
  apr_hash_t *deleted, *changed;
  apr_pool_t *subpool;

  /* Revision 0 is always empty. */
  if (rev == 0)
    {
      *mergeinfo = apr_hash_make(pool);
      return SVN_NO_ERROR;
    }

  subpool = svn_pool_create(pool);

  SVN_ERR(get_combined_mergeinfo(&curr_mergeinfo, fs, rev, paths, subpool));
  SVN_ERR(get_combined_mergeinfo(&prev_mergeinfo, fs, rev - 1, paths, subpool));

  SVN_ERR(svn_mergeinfo_diff(&deleted, &changed, prev_mergeinfo, curr_mergeinfo,
                             subpool));
  SVN_ERR(svn_mergeinfo_merge(&changed, deleted, subpool));

  *mergeinfo = svn_mergeinfo_dup(changed, pool);
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Same as send_change_rev(), but send all the revisions in RANGELIST.  Also,
   INCLUDE_MERGED_REVISIONS is assumed to be TRUE. */
static svn_error_t *
send_child_revs(const apr_array_header_t *paths,
                const apr_array_header_t *rangelist,
                svn_fs_t *fs,
                svn_boolean_t discover_changed_paths,
                svn_boolean_t omit_log_text,
                svn_boolean_t descending_order,
                svn_repos_authz_func_t authz_read_func,
                void *authz_read_baton,
                svn_log_message_receiver2_t receiver,
                void *receiver_baton,
                apr_pool_t *pool)
{
  apr_array_header_t *revs;
  apr_pool_t *subpool, *iterpool;
  int i;

  subpool = svn_pool_create(pool);

  SVN_ERR(svn_rangelist_to_revs(&revs, rangelist, subpool));
  if (descending_order)
    qsort(revs->elts, revs->nelts, revs->elt_size,
          svn_sort_compare_revisions);

  iterpool = svn_pool_create(subpool);
  for (i = 0; i < revs->nelts; i++)
    {
      svn_revnum_t rev = APR_ARRAY_IDX(revs, i, svn_revnum_t);

      svn_pool_clear(iterpool);
      SVN_ERR(send_change_rev(paths, rev, fs, discover_changed_paths, TRUE,
                              omit_log_text, descending_order,
                              authz_read_func, authz_read_baton,
                              receiver, receiver_baton, iterpool));
    }

  svn_pool_destroy(iterpool);
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

static svn_error_t *
send_change_rev(const apr_array_header_t *paths,
                svn_revnum_t rev,
                svn_fs_t *fs,
                svn_boolean_t discover_changed_paths,
                svn_boolean_t include_merged_revisions,
                svn_boolean_t omit_log_text,
                svn_boolean_t descending_order,
                svn_repos_authz_func_t authz_read_func,
                void *authz_read_baton,
                svn_log_message_receiver2_t receiver,
                void *receiver_baton,
                apr_pool_t *pool)
{
  svn_string_t *author, *date, *message;
  apr_hash_t *r_props, *changed_paths = NULL;
  svn_log_entry_t *log_entry;
  apr_uint64_t nbr_children = 0;
  apr_array_header_t *rangelist;
  apr_hash_t *mergeinfo;

  SVN_ERR(svn_fs_revision_proplist(&r_props, fs, rev, pool));
  author = apr_hash_get(r_props, SVN_PROP_REVISION_AUTHOR,
                        APR_HASH_KEY_STRING);
  date = apr_hash_get(r_props, SVN_PROP_REVISION_DATE,
                      APR_HASH_KEY_STRING);
  message = apr_hash_get(r_props, SVN_PROP_REVISION_LOG,
                         APR_HASH_KEY_STRING);

  /* Discover changed paths if the user requested them
     or if we need to check that they are readable. */
  if ((rev > 0)        
      && (authz_read_func || discover_changed_paths))
    {
      svn_fs_root_t *newroot;
      svn_error_t *patherr;

      SVN_ERR(svn_fs_revision_root(&newroot, fs, rev, pool));
      patherr = detect_changed(&changed_paths,
                               newroot, fs,
                               authz_read_func, authz_read_baton,
                               pool);

      if (patherr
          && patherr->apr_err == SVN_ERR_AUTHZ_UNREADABLE)
        {
          /* All changed-paths are unreadable, so clear all fields. */
          svn_error_clear(patherr);              
          changed_paths = NULL;
          author = NULL;
          date = NULL;
          message = NULL;
        }
      else if (patherr
               && patherr->apr_err == SVN_ERR_AUTHZ_PARTIALLY_READABLE)
        {
          /* At least one changed-path was unreadable, so omit the
             log message.  (The unreadable paths are already
             missing from the hash.) */
          svn_error_clear(patherr);
          message = NULL;
        }
      else if (patherr)
        return patherr;

      /* It may be the case that an authz func was passed in, but
         the user still doesn't want to see any changed-paths. */
      if (! discover_changed_paths)
        changed_paths = NULL;
    }

  /* Intentionally omit the log message if requested. */
  if (omit_log_text)
    message = NULL;

  /* Check to see if we need to include any extra merged revisions. */
  if (include_merged_revisions)
    {
      SVN_ERR(get_merged_rev_mergeinfo(&mergeinfo, fs, paths, rev, pool));
      SVN_ERR(combine_mergeinfo_rangelists(&rangelist, mergeinfo, pool));

      nbr_children = svn_rangelist_count_revs(rangelist);
    }

  log_entry = svn_log_entry_create(pool);

  log_entry->changed_paths = changed_paths;
  log_entry->revision = rev;
  log_entry->author = author ? author->data : NULL;
  log_entry->date = date ? date->data : NULL;
  log_entry->message = message ? message->data : NULL;
  log_entry->nbr_children = nbr_children;

  SVN_ERR((*receiver)(receiver_baton,
                      log_entry,
                      pool));

  if (nbr_children > 0)
    SVN_ERR(send_child_revs(paths, rangelist, fs, discover_changed_paths,
                            omit_log_text, descending_order,
                            authz_read_func, authz_read_baton, receiver,
                            receiver_baton, pool));

  return SVN_NO_ERROR;
}

/* This controls how many history objects we keep open.  For any targets
   over this number we have to open and close their histories as needed,
   which is CPU intensive, but keeps us from using an unbounded amount of
   memory. */
#define MAX_OPEN_HISTORIES 32

svn_error_t *
svn_repos_get_logs4(svn_repos_t *repos,
                    const apr_array_header_t *paths,
                    svn_revnum_t start,
                    svn_revnum_t end,
                    int limit,
                    svn_boolean_t discover_changed_paths,
                    svn_boolean_t strict_node_history,
                    svn_boolean_t include_merged_revisions,
                    svn_boolean_t omit_log_text,
                    svn_repos_authz_func_t authz_read_func,
                    void *authz_read_baton,
                    svn_log_message_receiver2_t receiver,
                    void *receiver_baton,
                    apr_pool_t *pool)
{
  svn_revnum_t head = SVN_INVALID_REVNUM;
  apr_pool_t *iterpool = svn_pool_create(pool);
  svn_fs_t *fs = repos->fs;
  apr_array_header_t *revs = NULL;
  svn_revnum_t hist_end = end;
  svn_revnum_t hist_start = start;
  svn_revnum_t current;
  apr_array_header_t *histories;
  svn_boolean_t any_histories_left = TRUE;
  svn_boolean_t descending_order;
  int send_count = 0;
  svn_fs_root_t *root;
  int i;

  SVN_ERR(svn_fs_youngest_rev(&head, fs, pool));

  if (! SVN_IS_VALID_REVNUM(start))
    start = head;

  if (! SVN_IS_VALID_REVNUM(end))
    end = head;

  /* Check that revisions are sane before ever invoking receiver. */
  if (start > head)
    return svn_error_createf
      (SVN_ERR_FS_NO_SUCH_REVISION, 0,
       _("No such revision %ld"), start);
  if (end > head)
    return svn_error_createf
      (SVN_ERR_FS_NO_SUCH_REVISION, 0,
       _("No such revision %ld"), end);

  descending_order = start >= end;

  /* Get an ordered copy of the start and end. */
  if (descending_order)
    {
      hist_start = end;
      hist_end = start;
    }

  /* If paths were specified, then we only really care about revisions
     in which those paths were changed.  So we ask the filesystem for
     all the revisions in which any of the paths was changed.

     SPECIAL CASE: If we were given only path, and that path is empty,
     then the results are the same as if we were passed no paths at
     all.  Why?  Because the answer to the question "In which
     revisions was the root of the filesystem changed?" is always
     "Every single one of them."  And since this section of code is
     only about answering that question, and we already know the
     answer ... well, you get the picture.
  */
  if (! paths ||
      (paths->nelts == 1 &&
       svn_path_is_empty(APR_ARRAY_IDX(paths, 0, const char *))))
    {
      /* They want history for the root path, so every rev has a change. */
      send_count = hist_end - hist_start + 1;
      if (limit && send_count > limit)
        send_count = limit;
      for (i = 0; i < send_count; ++i)
        {
          svn_revnum_t rev = hist_start + i;

          svn_pool_clear(iterpool);
          if (descending_order)
            rev = hist_end - i;
          SVN_ERR(send_change_rev(paths, rev, fs,
                                  discover_changed_paths,
                                  include_merged_revisions,
                                  omit_log_text, descending_order,
                                  authz_read_func, authz_read_baton,
                                  receiver, receiver_baton, iterpool));
        }
      svn_pool_destroy(iterpool);
      return SVN_NO_ERROR;
    }

  /* Create a history object for each path so we can walk through
     them all at the same time until we have all changes or LIMIT
     is reached.

     There is some pool fun going on due to the fact that we have
     to hold on to the old pool with the history before we can
     get the next history.
  */
  histories = apr_array_make(pool, paths->nelts,
                             sizeof(struct path_info *));

  SVN_ERR(svn_fs_revision_root(&root, fs, hist_end, pool));

  for (i = 0; i < paths->nelts; i++)
    {
      const char *this_path = APR_ARRAY_IDX(paths, i, const char *);
      struct path_info *info = apr_palloc(pool,
                                          sizeof(struct path_info));

      if (authz_read_func)
        {
          svn_boolean_t readable;

          svn_pool_clear(iterpool);

          SVN_ERR(authz_read_func(&readable, root, this_path,
                                  authz_read_baton, iterpool));
          if (! readable)
            return svn_error_create(SVN_ERR_AUTHZ_UNREADABLE, NULL, NULL);
        }

      info->path = svn_stringbuf_create(this_path, pool);
      info->done = FALSE;
      info->history_rev = hist_end;
      info->first_time = TRUE;

      if (i < MAX_OPEN_HISTORIES)
        {
          SVN_ERR(svn_fs_node_history(&info->hist, root, this_path, pool));
          info->newpool = svn_pool_create(pool);
          info->oldpool = svn_pool_create(pool);
        }
      else
        {
          info->hist = NULL;
          info->oldpool = NULL;
          info->newpool = NULL;
        }

      SVN_ERR(get_history(info, fs,
                          strict_node_history,
                          authz_read_func, authz_read_baton,
                          hist_start, pool));
      APR_ARRAY_PUSH(histories, struct path_info *) = info;
    }

  /* Loop through all the revisions in the range and add any
     where a path was changed to the array, or if they wanted
     history in reverse order just send it to them right away.
  */
  for (current = hist_end;
       current >= hist_start && any_histories_left;
       current = next_history_rev(histories))
    {
      svn_boolean_t changed = FALSE;
      any_histories_left = FALSE;

      svn_pool_clear(iterpool);
      for (i = 0; i < histories->nelts; i++)
        {
          struct path_info *info = APR_ARRAY_IDX(histories, i,
                                                 struct path_info *);

          /* Check history for this path in current rev. */
          SVN_ERR(check_history(&changed, info, fs, current,
                                strict_node_history,
                                authz_read_func, authz_read_baton,
                                hist_start, pool));
          if (! info->done)
            any_histories_left = TRUE;
        }

      /* If any of the paths changed in this rev then add or send it. */
      if (changed)
        {
          /* If they wanted it in reverse order we can send it completely
             streamily right now. */
          if (descending_order)
            {
              SVN_ERR(send_change_rev(paths, current, fs,
                                      discover_changed_paths,
                                      include_merged_revisions,
                                      omit_log_text, descending_order,
                                      authz_read_func, authz_read_baton,
                                      receiver, receiver_baton,
                                      iterpool));
              if (limit && ++send_count >= limit)
                break;
            }
          else
            {
              /* They wanted it in forward order, so we have to buffer up
                 a list of revs and process it later. */
              if (! revs)
                revs = apr_array_make(pool, 64, sizeof(svn_revnum_t));
              APR_ARRAY_PUSH(revs, svn_revnum_t) = current;
            }
        }
    }

  if (revs)
    {
      /* Work loop for processing the revisions we found since they wanted
         history in forward order. */
      for (i = 0; i < revs->nelts; ++i)
        {
          svn_pool_clear(iterpool);
          SVN_ERR(send_change_rev(paths, APR_ARRAY_IDX(revs, revs->nelts - i - 1,
                                                       svn_revnum_t),
                                  fs, discover_changed_paths,
                                  include_merged_revisions,
                                  omit_log_text, descending_order,
                                  authz_read_func, authz_read_baton,
                                  receiver, receiver_baton, iterpool));
          if (limit && i + 1 >= limit)
            break;
        }
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_get_logs3(svn_repos_t *repos,
                    const apr_array_header_t *paths,
                    svn_revnum_t start,
                    svn_revnum_t end,
                    int limit,
                    svn_boolean_t discover_changed_paths,
                    svn_boolean_t strict_node_history,
                    svn_repos_authz_func_t authz_read_func,
                    void *authz_read_baton,
                    svn_log_message_receiver_t receiver,
                    void *receiver_baton,
                    apr_pool_t *pool)
{
  svn_log_message_receiver2_t receiver2;
  void *receiver2_baton;

  svn_compat_wrap_log_receiver(&receiver2, &receiver2_baton,
                               receiver, receiver_baton,
                               pool);

  return svn_repos_get_logs4(repos, paths, start, end, limit,
                             discover_changed_paths, strict_node_history,
                             FALSE, FALSE, authz_read_func, authz_read_baton,
                             receiver2, receiver2_baton,
                             pool);
}

svn_error_t *
svn_repos_get_logs2(svn_repos_t *repos,
                    const apr_array_header_t *paths,
                    svn_revnum_t start,
                    svn_revnum_t end,
                    svn_boolean_t discover_changed_paths,
                    svn_boolean_t strict_node_history,
                    svn_repos_authz_func_t authz_read_func,
                    void *authz_read_baton,
                    svn_log_message_receiver_t receiver,
                    void *receiver_baton,
                    apr_pool_t *pool)
{
  return svn_repos_get_logs3(repos, paths, start, end, 0,
                             discover_changed_paths, strict_node_history,
                             authz_read_func, authz_read_baton, receiver,
                             receiver_baton, pool);
}


svn_error_t *
svn_repos_get_logs(svn_repos_t *repos,
                   const apr_array_header_t *paths,
                   svn_revnum_t start,
                   svn_revnum_t end,
                   svn_boolean_t discover_changed_paths,
                   svn_boolean_t strict_node_history,
                   svn_log_message_receiver_t receiver,
                   void *receiver_baton,
                   apr_pool_t *pool)
{
  return svn_repos_get_logs3(repos, paths, start, end, 0,
                             discover_changed_paths, strict_node_history,
                             NULL, NULL, /* no authz stuff */
                             receiver, receiver_baton, pool);
}
