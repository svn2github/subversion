/* hooks.c : running repository hooks and sentinels
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <apr_pools.h>
#include <apr_file_io.h>

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "repos.h"

/* In the code below, "hook" is sometimes used indiscriminately to
   mean either hook or sentinel.  */



/*** Hook drivers. ***/

/* NAME, CMD and ARGS are the name, path to and arguments for the hook
   program that is to be run.  If CHECK_EXITCODE is TRUE then the hook's
   exit status will be checked, and if an error occurred the hook's stderr
   output will be added to the returned error.  If CHECK_EXITCODE is FALSE
   the hook's exit status will be ignored. */
static svn_error_t *
run_hook_cmd (const char *name,
              const char *cmd,
              const char **args,
              svn_boolean_t check_exitcode,
              apr_pool_t *pool)
{
  apr_file_t *read_errhandle, *write_errhandle;
  apr_status_t apr_err;
  svn_error_t *err;
  int exitcode;
  apr_exit_why_e exitwhy;

  /* Create a pipe to access stderr of the child. */
  apr_err = apr_file_pipe_create(&read_errhandle, &write_errhandle, pool);
  if (apr_err)
    return svn_error_createf
      (apr_err, NULL, "can't create pipe for %s hook", cmd);

  err = svn_io_run_cmd (".", cmd, args, &exitcode, &exitwhy, FALSE,
                        NULL, NULL, write_errhandle, pool);

  /* This seems to be done automatically if we pass the third parameter of
     apr_procattr_child_in/out_set(), but svn_io_run_cmd()'s interface does
     not support those parameters. */
  apr_err = apr_file_close (write_errhandle);
  if (!err && apr_err)
    return svn_error_create
      (apr_err, NULL, "can't close write end of stderr pipe");

  /* Function failed. */
  if (err)
    {
      err = svn_error_createf
        (SVN_ERR_REPOS_HOOK_FAILURE, err, "failed to run %s hook", cmd);
    }

  if (!err && check_exitcode)
    {
      /* Command failed. */
      if (! APR_PROC_CHECK_EXIT (exitwhy) || exitcode != 0)
        {
          svn_stringbuf_t *error;

          /* Read the file's contents into a stringbuf, allocated in POOL. */
          SVN_ERR (svn_stringbuf_from_aprfile (&error, read_errhandle, pool));

          err = svn_error_createf
              (SVN_ERR_REPOS_HOOK_FAILURE, err,
               "%s hook failed with error output:\n%s",
               name, error->data);
        }
    }

  /* Hooks are fallible, and so hook failure is "expected" to occur at
     times.  When such a failure happens we still want to close the pipe */
  apr_err = apr_file_close (read_errhandle);
  if (!err && apr_err)
    return svn_error_create
      (apr_err, NULL, "can't close read end of stdout pipe");

  return err;
}


/* Run the start-commit hook for REPOS.  Use POOL for any temporary
   allocations.  If the hook fails, return SVN_ERR_REPOS_HOOK_FAILURE.  */
svn_error_t *
svn_repos__hooks_start_commit (svn_repos_t *repos,
                               const char *user,
                               apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *hook = svn_repos_start_commit_hook (repos, pool);

  if ((! svn_io_check_resolved_path (hook, &kind, pool)) 
      && (kind == svn_node_file))
    {
      const char *args[4];

      args[0] = hook;
      args[1] = svn_repos_path (repos, pool);
      args[2] = user;
      args[3] = NULL;

      SVN_ERR (run_hook_cmd ("start-commit", hook, args, TRUE, pool));
    }

  return SVN_NO_ERROR;
}


/* Run the pre-commit hook for REPOS.  Use POOL for any temporary
   allocations.  If the hook fails, return SVN_ERR_REPOS_HOOK_FAILURE.  */
svn_error_t  *
svn_repos__hooks_pre_commit (svn_repos_t *repos,
                             const char *txn_name,
                             apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *hook = svn_repos_pre_commit_hook (repos, pool);

  if ((! svn_io_check_resolved_path (hook, &kind, pool)) 
      && (kind == svn_node_file))
    {
      const char *args[4];

      args[0] = hook;
      args[1] = svn_repos_path (repos, pool);
      args[2] = txn_name;
      args[3] = NULL;

      SVN_ERR (run_hook_cmd ("pre-commit", hook, args, TRUE, pool));
    }

  return SVN_NO_ERROR;
}


/* Run the post-commit hook for REPOS.  Use POOL for any temporary
   allocations.  If the hook fails, run SVN_ERR_REPOS_HOOK_FAILURE.  */
svn_error_t  *
svn_repos__hooks_post_commit (svn_repos_t *repos,
                              svn_revnum_t rev,
                              apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *hook = svn_repos_post_commit_hook (repos, pool);

  if ((! svn_io_check_resolved_path (hook, &kind, pool)) 
      && (kind == svn_node_file))
    {
      const char *args[4];

      args[0] = hook;
      args[1] = svn_repos_path (repos, pool);
      args[2] = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, rev);
      args[3] = NULL;

      SVN_ERR (run_hook_cmd ("post-commit", hook, args, FALSE, pool));
    }

  return SVN_NO_ERROR;
}


/* Run the pre-revprop-change hook for REPOS.  Use POOL for any
   temporary allocations.  If the hook fails, return
   SVN_ERR_REPOS_HOOK_FAILURE.  */
svn_error_t  *
svn_repos__hooks_pre_revprop_change (svn_repos_t *repos,
                                     svn_revnum_t rev,
                                     const char *author,
                                     const char *name,
                                     const svn_string_t *value,
                                     apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *hook = svn_repos_pre_revprop_change_hook (repos, pool);

  if ((! svn_io_check_resolved_path (hook, &kind, pool)) 
      && (kind == svn_node_file))
    {
      const char *args[6];

      /* ### somehow pass VALUE as stdin to hook?! */

      args[0] = hook;
      args[1] = svn_repos_path (repos, pool);
      args[2] = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, rev);
      args[3] = author;
      args[4] = name;
      args[5] = NULL;

      SVN_ERR (run_hook_cmd ("pre-revprop-change", hook, args, TRUE, pool));
    }
  else
    {
      /* If the pre- hook doesn't exist at all, then default to
         MASSIVE PARANOIA.  Changing revision properties is a lossy
         operation; so unless the repository admininstrator has
         *deliberately* created the pre-hook, disallow all changes. */
      return 
        svn_error_create 
        (SVN_ERR_REPOS_DISABLED_FEATURE, NULL,
         "Repository has not been enabled to accept revision propchanges;\n"
         "ask the administrator to create a pre-revprop-change hook.");
    }

  return SVN_NO_ERROR;
}


/* Run the pre-revprop-change hook for REPOS.  Use POOL for any
   temporary allocations.  If the hook fails, return
   SVN_ERR_REPOS_HOOK_FAILURE.  */
svn_error_t  *
svn_repos__hooks_post_revprop_change (svn_repos_t *repos,
                                      svn_revnum_t rev,
                                      const char *author,
                                      const char *name,
                                      apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *hook = svn_repos_post_revprop_change_hook (repos, pool);
  
  if ((! svn_io_check_resolved_path (hook, &kind, pool)) 
      && (kind == svn_node_file))
    {
      const char *args[6];

      args[0] = hook;
      args[1] = svn_repos_path (repos, pool);
      args[2] = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, rev);
      args[3] = author;
      args[4] = name;
      args[5] = NULL;

      SVN_ERR (run_hook_cmd ("post-revprop-change", hook, args, FALSE, pool));
    }

  return SVN_NO_ERROR;
}



/* 
 * vim:ts=4:sw=4:expandtab:tw=80:fo=tcroq 
 * vim:isk=a-z,A-Z,48-57,_,.,-,> 
 * vim:cino=>1s,e0,n0,f0,{.5s,}0,^-.5s,=.5s,t0,+1s,c3,(0,u0,\:0
 */
