/*
 * commit.c:  wrappers around wc commit functionality.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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

/* ==================================================================== */



/*** Includes. ***/

#include <string.h>
#include <assert.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include "svn_wc.h"
#include "svn_ra.h"
#include "svn_delta.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_test.h"
#include "svn_io.h"

#include "client.h"



/* Hash value for FILES hash in the import routines. */
struct imported_file
{
  apr_pool_t *subpool;
  void *file_baton;
};


/* Apply PATH's contents (as a delta against the empty string) to
   FILE_BATON in EDITOR.  Use POOL for any temporary allocation.  */
static svn_error_t *
send_file_contents (const char *path,
                    void *file_baton,
                    const svn_delta_editor_t *editor,
                    apr_pool_t *pool)
{
  svn_stream_t *contents;
  svn_txdelta_window_handler_t handler;
  void *handler_baton;
  apr_file_t *f = NULL;
  apr_status_t apr_err;

  /* Get an apr file for PATH. */
  SVN_ERR (svn_io_file_open (&f, path, APR_READ, APR_OS_DEFAULT, pool));
  
  /* Get a readable stream of the file's contents. */
  contents = svn_stream_from_aprfile (f, pool);

  /* Get an editor func that wants to consume the delta stream. */
  SVN_ERR (editor->apply_textdelta (file_baton, pool,
                                    &handler, &handler_baton));

  /* Send the file's contents to the delta-window handler. */
  SVN_ERR (svn_txdelta_send_stream (contents, handler, handler_baton, pool));

  /* Close the file. */
  apr_err = apr_file_close (f);
  if (apr_err)
    return svn_error_createf
      (apr_err, 0, NULL, "error closing `%s'", path);

  return SVN_NO_ERROR;
}


/* Import file PATH as EDIT_PATH in the repository directory indicated
 * by DIR_BATON in EDITOR.  
 *
 * Accumulate file paths and their batons in FILES, which must be
 * non-null.  (These are used to send postfix textdeltas later).
 *
 * If NOTIFY_FUNC is non-null, invoke it with NOTIFY_BATON for each
 * file.  ### add mime-type (or at least binary) indicator to
 *            notify_func ###
 *
 * Use POOL for any temporary allocation.
 */
static svn_error_t *
import_file (apr_hash_t *files,
             svn_wc_notify_func_t notify_func,
             void *notify_baton,
             const svn_delta_editor_t *editor,
             void *dir_baton,
             const char *path,
             const char *edit_path,
             apr_pool_t *pool)
{
  void *file_baton;
  const char *mimetype;
  apr_pool_t *hash_pool = apr_hash_pool_get (files);
  apr_pool_t *subpool = svn_pool_create (hash_pool);
  const char *filepath = apr_pstrdup (hash_pool, path);
  struct imported_file *value = apr_palloc (hash_pool, sizeof (*value));
  svn_boolean_t executable;

  /* Add the file, using the pool from the FILES hash. */
  SVN_ERR (editor->add_file (edit_path, dir_baton, NULL, SVN_INVALID_REVNUM, 
                             subpool, &file_baton));

  /* If the file has a discernable mimetype, add that as a property to
     the file. */
  SVN_ERR (svn_io_detect_mimetype (&mimetype, path, pool));
  if (mimetype)
    SVN_ERR (editor->change_file_prop (file_baton, SVN_PROP_MIME_TYPE,
                                       svn_string_create (mimetype, pool), 
                                       pool));

  /* If the file is executable, add that as a property to the file. */
  SVN_ERR (svn_io_is_file_executable (&executable, path, pool));
  if (executable)
    SVN_ERR (editor->change_file_prop (file_baton, SVN_PROP_EXECUTABLE,
                                       svn_string_create ("", pool), 
                                       pool));
  
  if (notify_func)
    (*notify_func) (notify_baton,
                    path,
                    svn_wc_notify_commit_added,
                    svn_node_file,
                    mimetype,
                    svn_wc_notify_state_inapplicable,
                    svn_wc_notify_state_inapplicable,
                    SVN_INVALID_REVNUM);

  /* Finally, add the file's path and baton to the FILES hash. */
  value->subpool = subpool;
  value->file_baton = file_baton;
  apr_hash_set (files, filepath, APR_HASH_KEY_STRING, (void *)value);

  return SVN_NO_ERROR;
}
             

/* Import directory PATH into the repository directory indicated by
 * DIR_BATON in EDITOR.  ROOT_PATH is the path imported as the root
 * directory, so all edits are relative to that.
 *
 * Accumulate file paths and their batons in FILES, which must be
 * non-null.  (These are used to send postfix textdeltas later).
 *
 * If NOTIFY_FUNC is non-null, invoke it with NOTIFY_BATON for each
 * directory.
 *
 * EXCLUDES is a hash whose keys are absolute paths to exclude from
 * the import (values are unused).
 * 
 * Use POOL for any temporary allocation.  */
static svn_error_t *
import_dir (apr_hash_t *files,
            svn_wc_notify_func_t notify_func,
            void *notify_baton,
            const svn_delta_editor_t *editor, 
            void *dir_baton,
            const char *path,
            const char *edit_path,
            svn_boolean_t nonrecursive,
            apr_hash_t *excludes,
            apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);  /* iteration pool */
  apr_dir_t *dir;
  apr_finfo_t finfo;
  apr_status_t apr_err;
  apr_int32_t flags = APR_FINFO_TYPE | APR_FINFO_NAME;
  svn_error_t *err;

  SVN_ERR (svn_io_dir_open (&dir, path, pool));

  for (err = svn_io_dir_read (&finfo, flags, dir, subpool);
       err == SVN_NO_ERROR;
       svn_pool_clear (subpool),
         err = svn_io_dir_read (&finfo, flags, dir, subpool))
    {
      const char *this_path, *this_edit_path, *abs_path;

      if (finfo.filetype == APR_DIR)
        {
          /* Skip entries for this dir and its parent.  
             (APR promises that they'll come first, so technically
             this guard could be moved outside the loop.  But somehow
             that feels iffy. */
          if (finfo.name[0] == '.'
              && (finfo.name[1] == '\0'
                  || (finfo.name[1] == '.' && finfo.name[2] == '\0')))
            continue;

          /* If someone's trying to import a directory named the same
             as our administrative directories, that's probably not
             what they wanted to do.  Someday we can take an option to
             make these subdirs be silently ignored, but for now,
             seems safest to error. */
          if (strcmp (finfo.name, SVN_WC_ADM_DIR_NAME) == 0)
            return svn_error_createf
              (SVN_ERR_CL_ADM_DIR_RESERVED, 0, NULL,
               "cannot import directory named \"%s\" (in `%s')",
               finfo.name, path);
        }

      /* Typically, we started importing from ".", in which case
         edit_path is "".  So below, this_path might become "./blah",
         and this_edit_path might become "blah", for example. */
      this_path = svn_path_join (path, finfo.name, subpool);
      this_edit_path = svn_path_join (edit_path, finfo.name, subpool);

      /* If this is an excluded path, exclude it. */
      SVN_ERR (svn_path_get_absolute (&abs_path, this_path, subpool));
      if (apr_hash_get (excludes, abs_path, APR_HASH_KEY_STRING))
        continue;

      /* We only import subdirectories when we're doing a regular
         recursive import. */
      if ((finfo.filetype == APR_DIR) && (! nonrecursive))
        {
          void *this_dir_baton;

          /* Add the new subdirectory, getting a descent baton from
             the editor. */
          SVN_ERR (editor->add_directory (this_edit_path, dir_baton, 
                                          NULL, SVN_INVALID_REVNUM, subpool,
                                          &this_dir_baton));

          /* By notifying before the recursive call below, we display
             a directory add before displaying adds underneath the
             directory.  To do it the other way around, just move this
             after the recursive call. */
          if (notify_func)
            (*notify_func) (notify_baton,
                            this_path,
                            svn_wc_notify_commit_added,
                            svn_node_dir,
                            NULL,
                            svn_wc_notify_state_inapplicable,
                            svn_wc_notify_state_inapplicable,
                            SVN_INVALID_REVNUM);

          /* Recurse. */
          SVN_ERR (import_dir (files,
                               notify_func, notify_baton,
                               editor, this_dir_baton, 
                               this_path, this_edit_path, 
                               FALSE, excludes, subpool));

          /* Finally, close the sub-directory. */
          SVN_ERR (editor->close_directory (this_dir_baton, subpool));
        }
      else if (finfo.filetype == APR_REG)
        {
          /* Import a file. */
          SVN_ERR (import_file (files,
                                notify_func, notify_baton,
                                editor, dir_baton, 
                                this_path, this_edit_path, subpool));
        }
      /* We're silently ignoring things that aren't files or
         directories.  If we stop doing that, here is the place to
         change your world.  */
    }

  /* Check that the loop exited cleanly. */
  if (! (APR_STATUS_IS_ENOENT (err->apr_err)))
    return svn_error_createf
      (err->apr_err, err->src_err, err,
       "error during import of `%s'", path);

  /* Yes, it exited cleanly, so close the dir. */
  else if ((apr_err = apr_dir_close (dir)))
    return svn_error_createf
      (apr_err, 0, NULL, "error closing dir `%s'", path);
      
  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}


/* Recursively import PATH to a repository using EDITOR and
 * EDIT_BATON.  PATH can be a file or directory.
 * 
 * NEW_ENTRY is the name to use in the repository.  If PATH is a
 * directory, NEW_ENTRY may be null, which creates as many new entries
 * in the top repository target directory as there are entries in the
 * top of PATH; but if NEW_ENTRY is non-null, it is the name of a new
 * subdirectory in the repository to hold the import.  If PATH is a
 * file, NEW_ENTRY may not be null.
 * 
 * NEW_ENTRY can never be the empty string.
 * 
 * If NOTIFY_FUNC is non-null, invoke it with NOTIFY_BATON for each
 * imported path, passing the actions svn_wc_notify_commit_added or
 * svn_wc_notify_commit_postfix_txdelta.
 *
 * EXCLUDES is a hash whose keys are absolute paths to exclude from
 * the import (values are unused).
 * 
 * Use POOL for any temporary allocation.
 *
 * Note: the repository directory receiving the import was specified
 * when the editor was fetched.  (I.e, when EDITOR->open_root() is
 * called, it returns a directory baton for that directory, which is
 * not necessarily the root.)
 */
static svn_error_t *
import (const char *path,
        const char *new_entry,
        svn_wc_notify_func_t notify_func,
        void *notify_baton,
        const svn_delta_editor_t *editor,
        void *edit_baton,
        svn_boolean_t nonrecursive,
        apr_hash_t *excludes,
        apr_pool_t *pool)
{
  void *root_baton;
  svn_node_kind_t kind;
  apr_hash_t *files = apr_hash_make (pool);
  apr_hash_index_t *hi;

  /* Get a root dir baton.  We pass an invalid revnum to open_root
     to mean "base this on the youngest revision".  Should we have an
     SVN_YOUNGEST_REVNUM defined for these purposes? */
  SVN_ERR (editor->open_root (edit_baton, SVN_INVALID_REVNUM, 
                              pool, &root_baton));

  /* Import a file or a directory tree. */
  SVN_ERR (svn_io_check_path (path, &kind, pool));

  /* Note that there is no need to check whether PATH's basename is
     the same name that we reserve for our admistritave
     subdirectories.  It would be strange, but not illegal to import
     the contents of a directory of that name, because the directory's
     own name is not part of those contents.  Of course, if something
     underneath it also has our reserved name, then we'll error. */

  if (kind == svn_node_file)
    {
      if (! new_entry)
        return svn_error_create
          (SVN_ERR_NODE_UNKNOWN_KIND, 0, NULL,
           "new entry name required when importing a file");

      SVN_ERR (import_file (files,
                            notify_func, notify_baton,
                            editor, root_baton, 
                            path, new_entry, pool));
    }
  else if (kind == svn_node_dir)
    {
      void *new_dir_baton = NULL;

      /* Grab a new baton, making two we'll have to close. */
      if (new_entry)
        SVN_ERR (editor->add_directory (new_entry, root_baton,
                                        NULL, SVN_INVALID_REVNUM,
                                        pool, &new_dir_baton));

#if 0 /* Temporarily blocked out for consideration, see below. */
      /* If we activate this notification, then
       *
       *   $ svn import url://blah/blah [PATH_TO_IMPORT] [NEW_ENTRY_IN_REPOS]
       *
       * will lead off with a notification for PATH_TO_IMPORT
       * ("." by default).  This is technically accurate -- after all,
       * that dir is being imported -- but it's also kind of
       * redundant.  I'm not sure it really helps users to see it.  In
       * any case, the current test suite does not expect it.  And see
       *
       *   http://subversion.tigris.org/issues/show_bug.cgi?id=735
       *   http://subversion.tigris.org/issues/show_bug.cgi?id=736
       *
       * which are also about import notification paths.
       *
       * If we _are_ going to do this, perhaps the better way would be
       * to have import_dir(foo) notify for foo, instead of only
       * handling things underneath foo and requiring its caller
       * (i.e., this code right here) to notify for foo itself.
       */
      if (notify_func)
        (*notify_func) (notify_baton,
                        path,
                        svn_wc_notify_commit_added,
                        svn_node_dir,
                        NULL,
                        svn_wc_notify_state_inapplicable,
                        svn_wc_notify_state_inapplicable,
                        SVN_INVALID_REVNUM);
#endif /* 0 */

      SVN_ERR (import_dir 
               (files,
                notify_func, notify_baton,
                editor, new_dir_baton ? new_dir_baton : root_baton, 
                path, new_entry ? new_entry : "",
                nonrecursive, excludes, pool));

      /* Close one baton or two. */
      if (new_dir_baton)
        SVN_ERR (editor->close_directory (new_dir_baton, pool));
    }
  else if (kind == svn_node_none)
    {
      return svn_error_createf
        (SVN_ERR_NODE_UNKNOWN_KIND, 0, NULL,
         "'%s' does not exist.", path);  
    }

  SVN_ERR (editor->close_directory (root_baton, pool));

  /* Do post-fix textdeltas here! */
  for (hi = apr_hash_first (pool, files); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      struct imported_file *value;
      const char *full_path;
      
      apr_hash_this (hi, &key, NULL, &val);
      value = val;
      full_path = key;
      SVN_ERR (send_file_contents (full_path, value->file_baton, 
                                   editor, value->subpool));

      /* ### full_path is wrong, should be remainder when path is
         subtracted */
      if (notify_func)
        (*notify_func) (notify_baton,
                        full_path,
                        svn_wc_notify_commit_postfix_txdelta,
                        svn_node_file,
                        NULL,
                        svn_wc_notify_state_inapplicable,
                        svn_wc_notify_state_inapplicable,
                        SVN_INVALID_REVNUM);

      SVN_ERR (editor->close_file (value->file_baton, value->subpool));
      svn_pool_destroy (value->subpool);
    }

  SVN_ERR (editor->close_edit (edit_baton, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
get_ra_editor (void **ra_baton, 
               void **session,
               svn_ra_plugin_t **ra_lib,
               const svn_delta_editor_t **editor,
               void **edit_baton,
               svn_client_auth_baton_t *auth_baton,
               const char *base_url,
               const char *base_dir,
               svn_wc_adm_access_t *base_access,
               const char *log_msg,
               apr_array_header_t *commit_items,
               svn_revnum_t *committed_rev,
               const char **committed_date,
               const char **committed_author,
               svn_boolean_t is_commit,
               apr_pool_t *pool)
{
  /* Get the RA vtable that matches URL. */
  SVN_ERR (svn_ra_init_ra_libs (ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (ra_lib, *ra_baton, 
                                  base_url, pool));
  
  /* Open an RA session to URL. */
  SVN_ERR (svn_client__open_ra_session (session, *ra_lib,
                                        base_url, base_dir, base_access,
                                        commit_items, is_commit,
                                        is_commit, auth_baton, pool));
  
  /* Fetch RA commit editor. */
  return (*ra_lib)->get_commit_editor (*session, editor, edit_baton, 
                                       committed_rev, committed_date, 
                                       committed_author, log_msg);
}


/*** Public Interfaces. ***/

svn_error_t *
svn_client_import (svn_client_commit_info_t **commit_info,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   svn_client_auth_baton_t *auth_baton,
                   const char *path,
                   const char *url,
                   const char *new_entry,
                   svn_client_get_commit_log_t log_msg_func,
                   void *log_msg_baton,
                   svn_boolean_t nonrecursive,
                   apr_pool_t *pool)
{
  svn_error_t *err;
  const char *log_msg;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  void *ra_baton, *session;
  svn_ra_plugin_t *ra_lib;
  svn_revnum_t committed_rev = SVN_INVALID_REVNUM;
  const char *committed_date = NULL;
  const char *committed_author = NULL;
  apr_hash_t *excludes = apr_hash_make (pool);

  /* Sanity check: NEW_ENTRY can be null or non-empty, but it can't be
     empty. */
  if (new_entry && (strcmp (new_entry, "") == 0))
    return svn_error_create (SVN_ERR_FS_PATH_SYNTAX, 0, NULL,
                             "empty string is an invalid entry name");

  /* The repository doesn't know about the reserved. */
  if (new_entry && strcmp (new_entry, SVN_WC_ADM_DIR_NAME) == 0)
    return svn_error_createf
      (SVN_ERR_CL_ADM_DIR_RESERVED, 0, NULL,
       "the name \"%s\" is reserved and cannot be imported",
       SVN_WC_ADM_DIR_NAME);

  if (log_msg_func)
    {
      /* If there's a log message gatherer, create a temporary commit
         item array solely to help generate the log message.  The
         array is not used for the import itself. */

      svn_client_commit_item_t *item;
      const char *tmp_file;
      apr_array_header_t *commit_items 
        = apr_array_make (pool, 1, sizeof (item));
      
      item = apr_pcalloc (pool, sizeof (*item));
      item->path = apr_pstrdup (pool, path);
      item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
      (*((svn_client_commit_item_t **) apr_array_push (commit_items))) 
        = item;
      
      SVN_ERR ((*log_msg_func) (&log_msg, &tmp_file, commit_items, 
                                log_msg_baton, pool));
      if (! log_msg)
        return SVN_NO_ERROR;
      if (tmp_file)
        {
          const char *abs_path;
          SVN_ERR (svn_path_get_absolute (&abs_path, tmp_file, pool));
          apr_hash_set (excludes, abs_path, APR_HASH_KEY_STRING, (void *)1);
        }
    }
  else
    log_msg = "";

  /* We're importing to an RA layer. */
    {
      svn_node_kind_t kind;
      const char *base_dir = path;

      SVN_ERR (svn_io_check_path (path, &kind, pool));
      if (kind == svn_node_file)
        svn_path_split (path, &base_dir, NULL, pool);
      SVN_ERR (get_ra_editor (&ra_baton, &session, &ra_lib, 
                              &editor, &edit_baton, auth_baton, url, base_dir,
                              NULL, log_msg, NULL, &committed_rev,
                              &committed_date, &committed_author, 
                              FALSE, pool));
    }

  /* If an error occured during the commit, abort the edit and return
     the error.  We don't even care if the abort itself fails.  */
  if ((err = import (path, new_entry,
                     notify_func, notify_baton,
                     editor, edit_baton, nonrecursive, excludes, pool)))
    {
      editor->abort_edit (edit_baton, pool);
      return err;
    }

  /* Close the session. */
  SVN_ERR (ra_lib->close (session));

  /* Finally, fill in the commit_info structure. */
  *commit_info = svn_client__make_commit_info (committed_rev,
                                               committed_author,
                                               committed_date,
                                               pool);
  return SVN_NO_ERROR;
}


static svn_error_t *
remove_tmpfiles (apr_hash_t *tempfiles,
                 apr_pool_t *pool)
{
  apr_hash_index_t *hi;

  /* Split if there's nothing to be done. */
  if (! tempfiles)
    return SVN_NO_ERROR;

  /* Clean up any tempfiles. */
  for (hi = apr_hash_first (pool, tempfiles); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      svn_node_kind_t kind;

      apr_hash_this (hi, &key, NULL, &val);
      SVN_ERR (svn_io_check_path ((const char *)key, &kind, pool));
      if (kind == svn_node_file)
        SVN_ERR (svn_io_remove_file ((const char *)key, pool));
    }

  return SVN_NO_ERROR;
}



static svn_error_t *
reconcile_errors (svn_error_t *commit_err,
                  svn_error_t *unlock_err,
                  svn_error_t *bump_err,
                  svn_error_t *cleanup_err,
                  apr_pool_t *pool)
{
  svn_error_t *err;

  /* Early release (for good behavior). */
  if (! (commit_err || unlock_err || bump_err || cleanup_err))
    return SVN_NO_ERROR;

  /* If there was a commit error, start off our error chain with
     that. */
  if (commit_err)
    {
      commit_err = svn_error_quick_wrap 
        (commit_err, "Commit failed (details follow):");
      err = commit_err;
    }

  /* Else, create a new "general" error that will lead off the errors
     that follow. */
  else
    err = svn_error_create (SVN_ERR_BASE, 0, NULL,
                            "Commit succeeded, but other errors follow:");

  /* If there was an unlock error... */
  if (unlock_err)
    {
      /* Wrap the error with some headers. */
      unlock_err = svn_error_quick_wrap 
        (unlock_err, "Error unlocking locked dirs (details follow):");

      /* Append this error to the chain. */
      svn_error_compose (err, unlock_err);
    }

  /* If there was a bumping error... */
  if (bump_err)
    {
      /* Wrap the error with some headers. */
      bump_err = svn_error_quick_wrap 
        (bump_err, "Error bumping revisions post-commit (details follow):");

      /* Append this error to the chain. */
      svn_error_compose (err, bump_err);
    }

  /* If there was a cleanup error... */
  if (cleanup_err)
    {
      /* Wrap the error with some headers. */
      cleanup_err = svn_error_quick_wrap 
        (cleanup_err, "Error in post-commit clean-up (details follow):");

      /* Append this error to the chain. */
      svn_error_compose (err, cleanup_err);
    }

  return err;
}

/* Return TRUE if one of the first PROCESSED items in COMMIT_ITEMS is a
   parent of PATH, return FALSE otherwise. */
static svn_boolean_t
have_processed_parent (apr_array_header_t *commit_items,
                       int processed,
                       const char *path,
                       apr_pool_t *pool)
{
  int i;
  for (i = 0; i < processed && i < commit_items->nelts; ++i)
    {
      svn_client_commit_item_t *item
        = ((svn_client_commit_item_t **) commit_items->elts)[i];

      if (svn_path_is_child (item->path, path, pool))
        return TRUE;
    }
  return FALSE;
}

svn_error_t *
svn_client_commit (svn_client_commit_info_t **commit_info,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   svn_client_auth_baton_t *auth_baton,
                   const apr_array_header_t *targets,
                   svn_client_get_commit_log_t log_msg_func,
                   void *log_msg_baton,
                   svn_boolean_t nonrecursive,
                   apr_pool_t *pool)
{
  const svn_delta_editor_t *editor;
  void *edit_baton;
  void *ra_baton, *session;
  const char *log_msg;
  svn_revnum_t committed_rev = SVN_INVALID_REVNUM;
  const char *committed_date = NULL;
  const char *committed_author = NULL;
  svn_ra_plugin_t *ra_lib;
  const char *base_dir;
  const char *base_url;
  apr_array_header_t *rel_targets;
  apr_hash_t *committables, *tempfiles = NULL;
  svn_wc_adm_access_t *base_dir_access;
  apr_array_header_t *commit_items;
  svn_error_t *cmt_err = NULL, *unlock_err = NULL;
  svn_error_t *bump_err = NULL, *cleanup_err = NULL;
  svn_boolean_t commit_in_progress = FALSE;
  const char *display_dir = "";
  int i;

  /* Condense the target list. */
  SVN_ERR (svn_path_condense_targets (&base_dir, &rel_targets, targets, pool));

  /* If we calculated only a base_dir and no relative targets, this
     must mean that we are being asked to commit a single directory.
     In order to do this properly, we need to anchor our commit up one
     directory level, so long as our anchor is still a versioned
     directory. */
  if ((! rel_targets) || (! rel_targets->nelts))
    {
      const char *parent_dir, *name;

      SVN_ERR (svn_wc_get_actual_target (base_dir, &parent_dir, &name, pool));
      if (name)
        {
          /* Our new "grandfather directory" is the parent directory
             of the former one. */
          base_dir = apr_pstrdup (pool, parent_dir);

          /* Make the array if it wasn't already created. */
          if (! rel_targets)
            rel_targets = apr_array_make (pool, targets->nelts, sizeof (name));

          /* Now, push this name as a relative path to our new
             base directory. */
          (*((const char **)apr_array_push (rel_targets))) = name;
        }
    }

  SVN_ERR (svn_wc_adm_open (&base_dir_access, NULL, base_dir, TRUE, TRUE,
                            pool));

  /* One day we might support committing from multiple working copies, but
     we don't yet.  This check ensures that we don't silently commit a
     subset of the targets */
  for (i = 0; i < targets->nelts; ++i)
    {
      svn_wc_adm_access_t *adm_access;
      const char *target;
          SVN_ERR (svn_path_get_absolute (&target,
                                          ((const char **)targets->elts)[i],
                                          pool));
      SVN_ERR_W (svn_wc_adm_probe_retrieve (&adm_access, base_dir_access,
                                            target, pool),
                 "Are all the targets part of the same working copy?");
    }

  /* Crawl the working copy for commit items. */
  if ((cmt_err = svn_client__harvest_committables (&committables, 
                                                   base_dir_access,
                                                   rel_targets, 
                                                   nonrecursive,
                                                   pool)))
    goto cleanup;

  /* ### todo: Currently there should be only one hash entry, which
     has a hacked name until we have the entries files storing
     canonical repository URLs.  Then, the hacked name can go away
     and be replaced with a canonical repos URL, and from there we
     are poised to started handling nested working copies.  See
     http://subversion.tigris.org/issues/show_bug.cgi?id=960. */
  if (! ((commit_items = apr_hash_get (committables,
                                       SVN_CLIENT__SINGLE_REPOS_NAME, 
                                       APR_HASH_KEY_STRING))))
    goto cleanup;

  /* Go get a log message.  If an error occurs, or no log message is
     specified, abort the operation. */
  if (log_msg_func)
    {
      const char *tmp_file;
      cmt_err = (*log_msg_func)(&log_msg, &tmp_file, commit_items, 
                                log_msg_baton, pool);
      if (cmt_err || (! log_msg))
        goto cleanup;
    }
  else
    log_msg = "";

  /* Sort and condense our COMMIT_ITEMS. */
  if ((cmt_err = svn_client__condense_commit_items (&base_url,
                                                    commit_items,
                                                    pool)))
    goto cleanup;

    {
      svn_revnum_t head = SVN_INVALID_REVNUM;

      if ((cmt_err = get_ra_editor (&ra_baton, &session, &ra_lib, 
                                    &editor, &edit_baton, auth_baton,
                                    base_url, base_dir, base_dir_access,
                                    log_msg, commit_items, &committed_rev, 
                                    &committed_date, &committed_author, 
                                    TRUE, pool)))
        goto cleanup;

      /* Make a note that we have a commit-in-progress. */
      commit_in_progress = TRUE;

      /* ### Temporary: If we have any non-added directories with
         property mods, make sure those directories are up-to-date.
         Someday this should just be protected against by the server.  */
      for (i = 0; i < commit_items->nelts; i++)
        {
          svn_client_commit_item_t *item
            = ((svn_client_commit_item_t **) commit_items->elts)[i];
          if ((item->kind == svn_node_dir)
              && (item->state_flags & SVN_CLIENT_COMMIT_ITEM_PROP_MODS)
              && (! (item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD)))
            {
              if (! SVN_IS_VALID_REVNUM (head))
                {
                  if ((cmt_err = ra_lib->get_latest_revnum (session, &head)))
                    goto cleanup;
                }

              if (item->revision != head)
                {             
                  cmt_err = svn_error_createf 
                    (SVN_ERR_WC_NOT_UP_TO_DATE, 0, NULL,
                     "Cannot commit propchanges for directory '%s'",
                     item->path);
                  goto cleanup;
                }
            }
        }
    }

  /* Determine prefix to strip from the commit notify messages */
  if ((cmt_err = svn_path_get_absolute (&display_dir,
                                        display_dir, pool)))
    goto cleanup;
  display_dir = svn_path_get_longest_ancestor (display_dir, base_dir, pool);

  /* Perform the commit. */
  cmt_err = svn_client__do_commit (base_url, commit_items, base_dir_access,
                                   editor, edit_baton, 
                                   notify_func, notify_baton,
                                   display_dir,
                                   &tempfiles, pool);

  /* Make a note that our commit is finished. */
  commit_in_progress = FALSE;

  /* Bump the revision if the commit went well. */
  if (! cmt_err)
    {
      apr_pool_t *subpool = svn_pool_create (pool);

      for (i = 0; i < commit_items->nelts; i++)
        {
          svn_client_commit_item_t *item
            = ((svn_client_commit_item_t **) commit_items->elts)[i];
          svn_boolean_t recurse = FALSE;
          const char *adm_access_path;
          svn_wc_adm_access_t *adm_access;
          const svn_wc_entry_t *entry;

          if (item->kind == svn_node_dir)
            adm_access_path = item->path;
          else
            svn_path_split (item->path, &adm_access_path, NULL, pool);

          bump_err = svn_wc_adm_retrieve (&adm_access, base_dir_access,
                                          adm_access_path, pool);
          if (bump_err)
            {
              if (bump_err->apr_err == SVN_ERR_WC_NOT_LOCKED
                  && have_processed_parent (commit_items, i, item->path, pool))
                {
                  /* This happens when the item is a directory that is
                     deleted, and it has been processed as a child of an
                     earlier item. */
                  svn_error_clear (bump_err);
                  bump_err = SVN_NO_ERROR;
                  continue;
                }
              goto cleanup;
            }

          if ((bump_err = svn_wc_entry (&entry, item->path, adm_access, TRUE,
                                        pool)))
            goto cleanup;

          if (! entry
              && have_processed_parent (commit_items, i, item->path, pool))
            /* This happens when the item is a file that is deleted, and it
               has been processed as a child of an earlier item. */
            continue;

          if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD) 
              && (item->kind == svn_node_dir)
              && (item->copyfrom_url))
            recurse = TRUE;

          if ((bump_err = svn_wc_process_committed (item->path, adm_access,
                                                    recurse,
                                                    committed_rev, 
                                                    committed_date,
                                                    committed_author, 
                                                    item->wcprop_changes,
                                                    subpool)))
            break;

          /* Clear the per-iteration subpool. */
          svn_pool_clear (subpool);
        }

      /* Destroy the subpool (unless an error occurred, since we'll
         need to keep the error around for a little while longer). */
      if (! bump_err)
        svn_pool_destroy (subpool);
    }

  /* Close the RA session. */
  if ((cleanup_err = ra_lib->close (session)))
    goto cleanup;

  /* Sleep for one second to ensure timestamp integrity. */
  apr_sleep (APR_USEC_PER_SEC * 1);

 cleanup:
  /* Abort the commit if it is still in progress. */
  if (commit_in_progress)
    editor->abort_edit (edit_baton, pool); /* ignore return value */

  /* ### Under what conditions should we remove the locks? */
  unlock_err = svn_wc_adm_close (base_dir_access);

  /* Remove any outstanding temporary text-base files. */
  cleanup_err = remove_tmpfiles (tempfiles, pool);

  /* Fill in the commit_info structure */
  *commit_info = svn_client__make_commit_info (committed_rev, 
                                               committed_author, 
                                               committed_date, pool);

  return reconcile_errors (cmt_err, unlock_err, bump_err, cleanup_err, pool);
}
