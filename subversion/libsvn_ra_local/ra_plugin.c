/*
 * ra_plugin.c : the main RA module for local repository access
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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

#include "ra_local.h"
#include "svn_ra.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_pools.h"

#define APR_WANT_STRFUNC
#include <apr_want.h>

/*----------------------------------------------------------------*/

/** Callbacks **/

/* This routine is originally passed as a "hook" to the filesystem
   commit editor.  When we get here, the track-editor has already
   stored committed targets inside the baton.
   
   Loop over all committed target paths within BATON, calling the
   clients' close_func() with NEW_REV. */

/* (This is a routine of type svn_repos_commit_hook_t) */
static svn_error_t *
cleanup_commit (svn_revnum_t new_rev, void *baton)
{
  apr_hash_index_t *hi;

  /* Recover our hook baton: */
  svn_ra_local__commit_closer_t *closer = 
    (svn_ra_local__commit_closer_t *) baton;

  if (! closer->close_func)
    return SVN_NO_ERROR;

  for (hi = apr_hash_first (closer->pool, closer->committed_targets);
       hi;
       hi = apr_hash_next (hi))
    {
      char *path;
      apr_size_t ignored_len;
      void *val;
      svn_stringbuf_t path_str;
      enum svn_recurse_kind r;

      apr_hash_this (hi, (void *) &path, &ignored_len, &val);

      /* Oh yes, the flogging ritual, how could I forget. */
      path_str.data = path;
      path_str.len = strlen (path);
      r = (enum svn_recurse_kind) val;

      SVN_ERR (closer->close_func (closer->close_baton, &path_str, 
                                   (r == svn_recursive) ? TRUE : FALSE,
                                   new_rev));
    }

  return SVN_NO_ERROR;
}



/* The reporter vtable needed by do_update() */

static const svn_ra_reporter_t ra_local_reporter = 
{
  svn_repos_set_path,
  svn_repos_delete_path,
  svn_repos_finish_report,
  svn_repos_abort_report
};



/*----------------------------------------------------------------*/

/** The RA plugin routines **/


static svn_error_t *
open (void **session_baton,
      svn_stringbuf_t *repos_URL,
      svn_ra_callbacks_t *callbacks,
      void *callback_baton,
      apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *session;
  void *a, *auth_baton;
  svn_ra_username_authenticator_t *authenticator;

  /* Allocate and stash the session_baton args we have already. */
  session = apr_pcalloc (pool, sizeof(*session));
  session->pool = pool;
  session->repository_URL = repos_URL;
  
  /* Get the username by "pulling" it from the callbacks. */
  SVN_ERR (callbacks->get_authenticator (&a,
                                         &auth_baton, 
                                         SVN_RA_AUTH_USERNAME, 
                                         callback_baton, pool));

  authenticator = (svn_ra_username_authenticator_t *) a;

  SVN_ERR (authenticator->get_username (&(session->username),
                                        auth_baton, FALSE, pool));

  /* Look through the URL, figure out which part points to the
     repository, and which part is the path *within* the
     repository. */
  SVN_ERR (svn_ra_local__split_URL (&(session->repos_path),
                                    &(session->fs_path),
                                    session->repository_URL,
                                    session->pool));

  /* Open the filesystem at located at environment `repos_path' */
  SVN_ERR (svn_repos_open (&(session->fs),
                           session->repos_path->data,
                           session->pool));

  /* ### ra_local is not going to bother to store the username in the
     working copy.  This means that the username will always be
     fetched from getuid() or from a commandline arg, which is fine.

     The reason for this decision is that in ra_local, authentication
     and authorization are blurred; we'd have to use authorization as
     a *test* to decide if the authentication was valid.  And we
     certainly don't want to track every subsequent svn_fs_* call's
     error, just to decide if it's legitmate to store a username! */

  *session_baton = session;
  return SVN_NO_ERROR;
}



static svn_error_t *
close (void *session_baton)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  /* Close the repository filesystem, which will free any memory used
     by it. */
  SVN_ERR (svn_fs_close_fs (baton->fs));

  return SVN_NO_ERROR;
}




static svn_error_t *
get_latest_revnum (void *session_baton,
                   svn_revnum_t *latest_revnum)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_fs_youngest_rev (latest_revnum, baton->fs, baton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
get_dated_revision (void *session_baton,
                    svn_revnum_t *revision,
                    apr_time_t tm)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_repos_dated_revision (revision, baton->fs, tm, baton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
get_commit_editor (void *session_baton,
                   const svn_delta_edit_fns_t **editor,
                   void **edit_baton,
                   svn_stringbuf_t *log_msg,
                   svn_ra_get_wc_prop_func_t get_func,
                   svn_ra_set_wc_prop_func_t set_func,
                   svn_ra_close_commit_func_t close_func,
                   void *close_baton)
{
  svn_delta_edit_fns_t *commit_editor, *tracking_editor;
  const svn_delta_edit_fns_t *composed_editor;
  void *commit_editor_baton, *tracking_editor_baton, *composed_editor_baton;

  svn_ra_local__session_baton_t *sess_baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  /* Construct a Magick commit-hook baton */
  svn_ra_local__commit_closer_t *closer
    = apr_pcalloc (sess_baton->pool, sizeof(*closer));

  closer->pool = sess_baton->pool;
  closer->close_func = close_func;
  closer->set_func = set_func;
  closer->close_baton = close_baton;
  closer->committed_targets = apr_hash_make (sess_baton->pool);
                                         
  /* Get the repos commit-editor */     
  SVN_ERR (svn_ra_local__get_editor (&commit_editor, &commit_editor_baton,
                                     sess_baton,
                                     log_msg,
                                     cleanup_commit, closer, /* fs will call
                                                                when done.*/
                                     sess_baton->pool));

  /* Get the commit `tracking' editor, telling it to store committed
     targets inside our `closer' object, and NOT to bump revisions.
     (The FS editor will do this for us.)  */
  SVN_ERR (svn_delta_get_commit_track_editor (&tracking_editor,
                                              &tracking_editor_baton,
                                              sess_baton->pool,
                                              closer->committed_targets,
                                              SVN_INVALID_REVNUM,
                                              NULL, NULL));

  /* Set up a pipeline between the editors, creating a composed editor. */
  svn_delta_compose_editors (&composed_editor, &composed_editor_baton,
                             commit_editor, commit_editor_baton,
                             tracking_editor, tracking_editor_baton,
                             sess_baton->pool);

  /* Give the magic composed-editor back to the client */
  *editor = composed_editor;
  *edit_baton = composed_editor_baton;
  return SVN_NO_ERROR;
}



/* todo: the fs_path inside session_baton is currently in
   svn_path_url_style.  To be *formally* correct, this routine needs
   to dup that path and convert it to svn_path_repos_style.  That's
   the style that svn_ra_local__checkout expects in its starting path.
   We punt on this for now, since the two styles are equal at the
   moment. */
static svn_error_t *
do_checkout (void *session_baton,
             svn_revnum_t revision,
             svn_boolean_t recurse,
             const svn_delta_edit_fns_t *editor,
             void *edit_baton)
{
  svn_revnum_t revnum_to_fetch;
  svn_ra_local__session_baton_t *sbaton = 
    (svn_ra_local__session_baton_t *) session_baton;
  
  if (! SVN_IS_VALID_REVNUM(revision))
    SVN_ERR (get_latest_revnum (sbaton, &revnum_to_fetch));
  else
    revnum_to_fetch = revision;

  SVN_ERR (svn_ra_local__checkout (sbaton->fs,
                                   revnum_to_fetch,
                                   recurse,
                                   sbaton->repository_URL,
                                   sbaton->fs_path,
                                   editor, edit_baton, sbaton->pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
do_update (void *session_baton,
           const svn_ra_reporter_t **reporter,
           void **report_baton,
           svn_revnum_t update_revision,
           svn_stringbuf_t *update_target,
           svn_boolean_t recurse,
           const svn_delta_edit_fns_t *update_editor,
           void *update_baton)
{
  svn_revnum_t revnum_to_update_to;
  svn_ra_local__session_baton_t *sbaton = session_baton;
  
  if (! SVN_IS_VALID_REVNUM(update_revision))
    SVN_ERR (get_latest_revnum (sbaton, &revnum_to_update_to));
  else
    revnum_to_update_to = update_revision;

  /* Pass back our reporter */
  *reporter = &ra_local_reporter;

  /* Build a reporter baton. */
  return svn_repos_begin_report (report_baton,
                                 revnum_to_update_to,
                                 sbaton->username,
                                 sbaton->fs, sbaton->fs_path,
                                 update_target, TRUE,
                                 recurse,
                                 update_editor, update_baton,
                                 sbaton->pool);
}


static svn_error_t *
do_status (void *session_baton,
           const svn_ra_reporter_t **reporter,
           void **report_baton,
           svn_stringbuf_t *status_target,
           svn_boolean_t recurse,
           const svn_delta_edit_fns_t *status_editor,
           void *status_baton)
{
  svn_revnum_t revnum_to_update_to;
  svn_ra_local__session_baton_t *sbaton = session_baton;
  
  SVN_ERR (get_latest_revnum (sbaton, &revnum_to_update_to));

  /* Pass back our reporter */
  *reporter = &ra_local_reporter;

  /* Build a reporter baton. */
  return svn_repos_begin_report (report_baton,
                                 revnum_to_update_to,
                                 sbaton->username,
                                 sbaton->fs, sbaton->fs_path,
                                 status_target, FALSE,
                                 recurse,
                                 status_editor, status_baton,
                                 sbaton->pool);
}


static svn_error_t *
get_log (void *session_baton,
         const apr_array_header_t *paths,
         svn_revnum_t start,
         svn_revnum_t end,
         svn_boolean_t discover_changed_paths,
         svn_log_message_receiver_t receiver,
         void *receiver_baton)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  apr_array_header_t *abs_paths
    = apr_array_make (sbaton->pool, paths->nelts, sizeof (svn_stringbuf_t *));
  int i;

  for (i = 0; i < paths->nelts; i++)
    {
      svn_stringbuf_t *relative_path
        = (((svn_stringbuf_t **)(paths)->elts)[i]);

      svn_stringbuf_t *abs_path
        = svn_stringbuf_dup (sbaton->fs_path, sbaton->pool);

      /* ### Not sure if this counts as a workaround or not.  The
         session baton uses the empty string to mean root, and not
         sure that should change.  However, it would be better to use
         a path library function to add this separator -- hardcoding
         it is totally bogus.  See issue #559, though it may be only
         tangentially related. */
      if (abs_path->len == 0)
        svn_stringbuf_appendcstr (abs_path, "/");

      svn_path_add_component (abs_path, relative_path,
                              svn_path_repos_style);
      (*((svn_stringbuf_t **)(apr_array_push (abs_paths)))) = abs_path;
    }

  return svn_repos_get_logs (sbaton->fs,
                             abs_paths,
                             start,
                             end,
                             discover_changed_paths,
                             receiver,
                             receiver_baton,
                             sbaton->pool);
}


static svn_error_t *
do_check_path (svn_node_kind_t *kind,
               void *session_baton,
               const char *path,
               svn_revnum_t revision)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  svn_fs_root_t *root;
  svn_stringbuf_t *abs_path 
    = svn_stringbuf_dup (sbaton->fs_path, sbaton->pool);

  /* ### Not sure if this counts as a workaround or not.  The
     session baton uses the empty string to mean root, and not
     sure that should change.  However, it would be better to use
     a path library function to add this separator -- hardcoding
     it is totally bogus.  See issue #559, though it may be only
     tangentially related. */
  if (abs_path->len == 0)
    svn_stringbuf_appendcstr (abs_path, "/");

  /* If we were given a relative path to append, append it. */
  if (path)
    svn_path_add_component_nts (abs_path, path, svn_path_repos_style);

  if (! SVN_IS_VALID_REVNUM (revision))
    SVN_ERR (svn_fs_youngest_rev (&revision, sbaton->fs, sbaton->pool));
  SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, revision, sbaton->pool));
  *kind = svn_fs_check_path (root, abs_path->data, sbaton->pool);
  return SVN_NO_ERROR;
}



/* Getting just one file. */

static svn_error_t *
get_file (void *session_baton,
          svn_stringbuf_t *path,
          svn_revnum_t revision,
          svn_stream_t *stream)
{

#if 0
  svn_stream_t *contents;

  SVN_ERR (svn_fs_file_contents (&contents
                                 svn_fs_root_t *root,
                                 const char *path,
                                 apr_pool_t *pool);

#endif /* 0 */

  abort ();
  return SVN_NO_ERROR;
}


/*----------------------------------------------------------------*/

/** The ra_plugin **/

static const svn_ra_plugin_t ra_local_plugin = 
{
  "ra_local",
  "Module for accessing a repository on local disk.",
  open,
  close,
  get_latest_revnum,
  get_dated_revision,
  get_commit_editor,
  get_file,
  do_checkout,
  do_update,
  do_status,
  get_log,
  do_check_path
};


/*----------------------------------------------------------------*/

/** The One Public Routine, called by libsvn_client **/

svn_error_t *
svn_ra_local_init (int abi_version,
                   apr_pool_t *pool,
                   apr_hash_t *hash)
{
  apr_hash_set (hash, "file", APR_HASH_KEY_STRING, &ra_local_plugin);

  /* ben sez:  todo:  check that abi_version >=1. */

  return SVN_NO_ERROR;
}








/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
