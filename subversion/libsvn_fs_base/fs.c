/* fs.c --- creating, opening and closing filesystems
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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
#include <stdio.h>
#include <string.h>
#include <errno.h>              /* for EINVAL */

#define APU_WANT_DB
#include <apu_want.h>

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_file_io.h>

#include "svn_pools.h"
#include "svn_fs.h"
#include "svn_path.h"
#include "svn_utf.h"
#include "fs.h"
#include "err.h"
#include "dag.h"
#include "revs-txns.h"
#include "uuid.h"
#include "tree.h"
#include "id.h"
#include "svn_private_config.h"

#include "bdb/bdb-err.h"
#include "bdb/bdb_compat.h"
#include "bdb/nodes-table.h"
#include "bdb/rev-table.h"
#include "bdb/txn-table.h"
#include "bdb/copies-table.h"
#include "bdb/changes-table.h"
#include "bdb/reps-table.h"
#include "bdb/strings-table.h"
#include "bdb/uuids-table.h"

#include "../libsvn_fs/fs-loader.h"


/* Checking for return values, and reporting errors.  */

/* Check that we're using the right Berkeley DB version. */
/* FIXME: This check should be abstracted into the DB back-end layer. */
static svn_error_t *
check_bdb_version (apr_pool_t *pool)
{
  int major, minor, patch;

  db_version (&major, &minor, &patch);

  /* First, check that we're using a reasonably correct of Berkeley DB. */
  if ((major < SVN_FS_WANT_DB_MAJOR)
      || (major == SVN_FS_WANT_DB_MAJOR && minor < SVN_FS_WANT_DB_MINOR)
      || (major == SVN_FS_WANT_DB_MAJOR && minor == SVN_FS_WANT_DB_MINOR
          && patch < SVN_FS_WANT_DB_PATCH))
    return svn_error_createf (SVN_ERR_FS_GENERAL, 0,
                              "Bad database version: got %d.%d.%d,"
                              " should be at least %d.%d.%d",
                              major, minor, patch,
                              SVN_FS_WANT_DB_MAJOR,
                              SVN_FS_WANT_DB_MINOR,
                              SVN_FS_WANT_DB_PATCH);

  /* Now, check that the version we're running against is the same as
     the one we compiled with. */
  if (major != DB_VERSION_MAJOR || minor != DB_VERSION_MINOR)
    return svn_error_createf (SVN_ERR_FS_GENERAL, 0,
                              "Bad database version:"
                              " compiled with %d.%d.%d,"
                              " running against %d.%d.%d",
                              DB_VERSION_MAJOR,
                              DB_VERSION_MINOR,
                              DB_VERSION_PATCH,
                              major, minor, patch);
  return SVN_NO_ERROR;
}


/* If FS is already open, then return an SVN_ERR_FS_ALREADY_OPEN
   error.  Otherwise, return zero.  */
static svn_error_t *
check_already_open (svn_fs_t *fs)
{
  if (fs->fsap_data)
    return svn_error_create (SVN_ERR_FS_ALREADY_OPEN, 0,
                             "Filesystem object already open");
  else
    return SVN_NO_ERROR;
}


/* Cleanup functions.  */

/* Close a database in the filesystem FS.
   DB_PTR is a pointer to the DB pointer in *FS to close.
   NAME is the name of the database, for use in error messages.  */
static svn_error_t *
cleanup_fs_db (svn_fs_t *fs, DB **db_ptr, const char *name)
{
  if (*db_ptr)
    {
      DB *db = *db_ptr;
      char *msg = apr_psprintf (fs->pool, "closing '%s' database", name);
      int db_err;

      *db_ptr = 0;
      db_err = db->close (db, 0);

#if SVN_BDB_HAS_DB_INCOMPLETE
      /* We can ignore DB_INCOMPLETE on db->close and db->sync; it
       * just means someone else was using the db at the same time
       * we were.  See the Berkeley documentation at:
       * http://www.sleepycat.com/docs/ref/program/errorret.html#DB_INCOMPLETE
       * http://www.sleepycat.com/docs/api_c/db_close.html
       */
      if (db_err == DB_INCOMPLETE)
        db_err = 0;
#endif /* SVN_BDB_HAS_DB_INCOMPLETE */

      SVN_ERR (BDB_WRAP (fs, msg, db_err));
    }

  return SVN_NO_ERROR;
}

/* Close whatever Berkeley DB resources are allocated to FS.  */
static svn_error_t *
cleanup_fs (svn_fs_t *fs)
{
  base_fs_data_t *bfd = fs->fsap_data;
  DB_ENV *env = bfd ? bfd->env : NULL;

  if (! env)
    return SVN_NO_ERROR;

  /* Close the databases.  */
  SVN_ERR (cleanup_fs_db (fs, &bfd->nodes, "nodes"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->revisions, "revisions"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->transactions, "transactions"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->copies, "copies"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->changes, "changes"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->representations, "representations"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->strings, "strings"));
  SVN_ERR (cleanup_fs_db (fs, &bfd->uuids, "uuids"));

  /* Finally, close the environment.  */
  bfd->env = 0;
  SVN_ERR (BDB_WRAP (fs, "closing environment",
                    env->close (env, 0)));

  return SVN_NO_ERROR;
}

#if 0   /* Set to 1 for instrumenting. */
static void print_fs_stats(svn_fs_t *fs)
{
  base_fs_data_t *bfd = fs->fsap_data;
  DB_TXN_STAT *t;
  DB_LOCK_STAT *l;
  int db_err;

  /* Print transaction statistics for this DB env. */
  if ((db_err = bfd->env->txn_stat (bfd->env, &t, 0)) != 0)
    fprintf (stderr, "Error running bfd->env->txn_stat(): %s",
             db_strerror (db_err));
  else
    {
      printf ("*** DB transaction stats, right before closing env:\n");
      printf ("   Number of transactions currently active: %d\n",
              t->st_nactive);
      printf ("   Max number of active transactions at any one time: %d\n",
              t->st_maxnactive);
      printf ("   Number of transactions that have begun: %d\n",
              t->st_nbegins);
      printf ("   Number of transactions that have aborted: %d\n",
              t->st_naborts);
      printf ("   Number of transactions that have committed: %d\n",
              t->st_ncommits);
      printf ("   Number of times a thread was forced to wait: %d\n",
              t->st_region_wait);
      printf ("   Number of times a thread didn't need to wait: %d\n",
              t->st_region_nowait);
      printf ("*** End DB transaction stats.\n\n");
    }

  /* Print transaction statistics for this DB env. */
  if ((db_err = bfd->env->lock_stat (bfd->env, &l, 0)) != 0)
    fprintf (stderr, "Error running bfd->env->lock_stat(): %s",
             db_strerror (db_err));
  else
    {
      printf ("*** DB lock stats, right before closing env:\n");
      printf ("   The number of current locks: %d\n",
              l->st_nlocks);
      printf ("   Max number of locks at any one time: %d\n",
              l->st_maxnlocks);
      printf ("   Number of current lockers: %d\n",
              l->st_nlockers);
      printf ("   Max number of lockers at any one time: %d\n",
              l->st_maxnlockers);
      printf ("   Number of current objects: %d\n",
              l->st_nobjects);
      printf ("   Max number of objects at any one time: %d\n",
              l->st_maxnobjects);
      printf ("   Total number of locks requested: %d\n",
              l->st_nrequests);
      printf ("   Total number of locks released: %d\n",
              l->st_nreleases);
      printf ("   Total number of lock reqs failed because "
              "DB_LOCK_NOWAIT was set: %d\n", l->st_nnowaits);
      printf ("   Total number of locks not immediately available "
              "due to conflicts: %d\n", l->st_nconflicts);
      printf ("   Number of deadlocks detected: %d\n", l->st_ndeadlocks);
      printf ("   Number of times a thread waited before "
              "obtaining the region lock: %d\n", l->st_region_wait);
      printf ("   Number of times a thread didn't have to wait: %d\n",
              l->st_region_nowait);
      printf ("*** End DB lock stats.\n\n");
    }

}
#else
#  define print_fs_stats(fs)
#endif /* 0/1 */

/* An APR pool cleanup function for a filesystem.  DATA must be a
   pointer to the filesystem to clean up.

   When the filesystem object's pool is freed, we want the resources
   held by Berkeley DB to go away, just like everything else.  So we
   register this cleanup function with the filesystem's pool, and let
   it take care of closing the databases, the environment, and any
   other DB objects we might be using.  APR calls this function before
   actually freeing the pool's memory.

   It's a pity that we can't return an svn_error_t object from an APR
   cleanup function.  For now, we return the rather generic
   SVN_ERR_FS_CLEANUP, and pass the real svn_error_t to the registered
   warning callback.  */

static apr_status_t
cleanup_fs_apr (void *data)
{
  svn_fs_t *fs = data;
  svn_error_t *err;

  print_fs_stats (fs);

  err = cleanup_fs (fs);
  if (! err)
    return APR_SUCCESS;

  /* Darn. An error during cleanup. Call the warning handler to
     try and do something "right" with this error. Note that
     the default will simply abort().  */
  (*fs->warning) (fs->warning_baton, err);

  svn_error_clear (err);

  return SVN_ERR_FS_CLEANUP;
}



static svn_error_t *
base_bdb_set_errcall (svn_fs_t *fs,
                      void (*db_errcall_fcn) (const char *errpfx, char *msg))
{
  base_fs_data_t *bfd = fs->fsap_data;

  SVN_ERR (svn_fs_base__check_fs (fs));
  bfd->env->set_errcall(bfd->env, db_errcall_fcn);

  return SVN_NO_ERROR;
}



/* Allocating an appropriate Berkeley DB environment object.  */

/* Allocate a Berkeley DB environment object for the filesystem FS,
   and set up its default parameters appropriately.  */
static svn_error_t *
allocate_env (svn_fs_t *fs)
{
  base_fs_data_t *bfd = fs->fsap_data;

  /* Allocate a Berkeley DB environment object.  */
  SVN_ERR (BDB_WRAP (fs, "allocating environment object",
                     db_env_create (&bfd->env, 0)));

  /* Needed on Windows in case Subversion and Berkeley DB are using
     different C runtime libraries  */
  SVN_ERR (BDB_WRAP (fs, "setting environment object's allocation functions",
                     bfd->env->set_alloc (bfd->env, malloc, realloc, free)));

  /* If we detect a deadlock, select a transaction to abort at random
     from those participating in the deadlock.  */
  SVN_ERR (BDB_WRAP (fs, "setting deadlock detection policy",
                     bfd->env->set_lk_detect (bfd->env, DB_LOCK_RANDOM)));

  return SVN_NO_ERROR;
}



/* Write the DB_CONFIG file. */
static svn_error_t *
bdb_write_config  (svn_fs_t *fs)
{
  const char *dbconfig_file_name =
    svn_path_join (fs->path, "DB_CONFIG", fs->pool);
  apr_file_t *dbconfig_file = NULL;
  int i;

  static const char dbconfig_contents[] =
    "# This is the configuration file for the Berkeley DB environment\n"
    "# used by your Subversion repository.\n"
    "# You must run 'svnadmin recover' whenever you modify this file,\n"
    "# for your changes to take effect.\n"
    "\n"
    "### Lock subsystem\n"
    "#\n"
    "# Make sure you read the documentation at:\n"
    "#\n"
    "#   http://www.sleepycat.com/docs/ref/lock/max.html\n"
    "#\n"
    "# before tweaking these values.\n"
    "set_lk_max_locks   2000\n"
    "set_lk_max_lockers 2000\n"
    "set_lk_max_objects 2000\n"
    "\n"
    "### Log file subsystem\n"
    "#\n"
    "# Make sure you read the documentation at:\n"
    "#\n"
    "#   http://www.sleepycat.com/docs/api_c/env_set_lg_bsize.html\n"
    "#   http://www.sleepycat.com/docs/api_c/env_set_lg_max.html\n"
    "#   http://www.sleepycat.com/docs/ref/log/limits.html\n"
    "#\n"
    "# Increase the size of the in-memory log buffer from the default\n"
    "# of 32 Kbytes to 256 Kbytes.  Decrease the log file size from\n"
    "# 10 Mbytes to 1 Mbyte.  This will help reduce the amount of disk\n"
    "# space required for hot backups.  The size of the log file must be\n"
    "# at least four times the size of the in-memory log buffer.\n"
    "#\n"
    "# Note: Decreasing the in-memory buffer size below 256 Kbytes\n"
    "# will hurt commit performance. For details, see this post from\n"
    "# Daniel Berlin <dan@dberlin.org>:\n"
    "#\n"
    "# http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgId=161960\n"
    "set_lg_bsize     262144\n"
    "set_lg_max      1048576\n";

  /* Run-time configurable options.
     Each option set consists of a minimum required BDB version, a
     config hash key, a header, an inactive form and an active
     form. We always write the header; then, depending on the
     run-time configuration and the BDB version we're compiling
     against, we write either the active or inactive form of the
     value. */
  static const struct
  {
    int bdb_major;
    int bdb_minor;
    const char *config_key;
    const char *header;
    const char *inactive;
    const char *active;
  } dbconfig_options[] = {
    /* Controlled by "svnadmin create --bdb-txn-nosync" */
    { 4, 0, SVN_FS_CONFIG_BDB_TXN_NOSYNC,
      /* header */
      "#\n"
      "# Disable fsync of log files on transaction commit. Read the\n"
      "# documentation about DB_TXN_NOSYNC at:\n"
      "#\n"
      "#   http://www.sleepycat.com/docs/api_c/env_set_flags.html\n"
      "#\n"
      "# [requires Berkeley DB 4.0]\n",
      /* inactive */
      "# set_flags DB_TXN_NOSYNC\n",
      /* active */
      "set_flags DB_TXN_NOSYNC\n" },
    /* Controlled by "svnadmin create --bdb-log-keep" */
    { 4, 2, SVN_FS_CONFIG_BDB_LOG_AUTOREMOVE,
      /* header */
      "#\n"
      "# Enable automatic removal of unused transaction log files.\n"
      "# Read the documentation about DB_LOG_AUTOREMOVE at:\n"
      "#\n"
      "#   http://www.sleepycat.com/docs/api_c/env_set_flags.html\n"
      "#\n"
      "# [requires Berkeley DB 4.2]\n",
      /* inactive */
      "# set_flags DB_LOG_AUTOREMOVE\n",
      /* active */
      "set_flags DB_LOG_AUTOREMOVE\n" },
  };
  static const int dbconfig_options_length =
    sizeof (dbconfig_options)/sizeof (*dbconfig_options);


  SVN_ERR (svn_io_file_open (&dbconfig_file, dbconfig_file_name,
                             APR_WRITE | APR_CREATE, APR_OS_DEFAULT,
                             fs->pool));

  SVN_ERR (svn_io_file_write_full (dbconfig_file, dbconfig_contents,
                                   sizeof (dbconfig_contents) - 1, NULL,
                                   fs->pool));

  /* Write the variable DB_CONFIG flags. */
  for (i = 0; i < dbconfig_options_length; ++i)
    {
      void *value = NULL;
      const char *choice;

      if (fs->config)
        {
          value = apr_hash_get (fs->config,
                                dbconfig_options[i].config_key,
                                APR_HASH_KEY_STRING);
        }

      SVN_ERR(svn_io_file_write_full (dbconfig_file,
                                      dbconfig_options[i].header,
                                      strlen (dbconfig_options[i].header),
                                      NULL, fs->pool));

      if (((DB_VERSION_MAJOR == dbconfig_options[i].bdb_major
            && DB_VERSION_MINOR >= dbconfig_options[i].bdb_minor)
           || DB_VERSION_MAJOR > dbconfig_options[i].bdb_major)
          && value != NULL && strcmp (value, "0") != 0)
        choice = dbconfig_options[i].active;
      else
        choice = dbconfig_options[i].inactive;

      SVN_ERR (svn_io_file_write_full (dbconfig_file, choice, strlen (choice),
                                       NULL, fs->pool));
    }

  SVN_ERR (svn_io_file_close (dbconfig_file, fs->pool));

  return SVN_NO_ERROR;
}



/* Creating a new filesystem */

static fs_vtable_t fs_vtable = {
  svn_fs_base__youngest_rev,
  svn_fs_base__revision_prop,
  svn_fs_base__revision_proplist,
  svn_fs_base__change_rev_prop,
  svn_fs_base__get_uuid,
  svn_fs_base__set_uuid,
  svn_fs_base__revision_root,
  svn_fs_base__begin_txn,
  svn_fs_base__open_txn,
  svn_fs_base__purge_txn,
  svn_fs_base__list_transactions,
  svn_fs_base__deltify
};

static svn_error_t *
base_create (svn_fs_t *fs, const char *path, apr_pool_t *pool)
{
  svn_error_t *svn_err;
  const char *path_apr;
  const char *path_native;
  base_fs_data_t *bfd;

  SVN_ERR (check_bdb_version (pool));
  SVN_ERR (check_already_open (fs));

  apr_pool_cleanup_register (fs->pool, fs, cleanup_fs_apr,
                             apr_pool_cleanup_null);

  bfd = apr_pcalloc (fs->pool, sizeof (*bfd));
  fs->vtable = &fs_vtable;
  fs->fsap_data = bfd;

  /* Initialize the fs's path. */
  fs->path = apr_pstrdup (fs->pool, path);
  SVN_ERR (svn_path_cstring_from_utf8 (&path_apr, fs->path, fs->pool));

  SVN_ERR (bdb_write_config (fs));

  svn_err = allocate_env (fs);
  if (svn_err) goto error;

  /* Create the Berkeley DB environment.  */
  SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, fs->path, fs->pool));
  svn_err = BDB_WRAP (fs, "creating environment",
                      bfd->env->open (bfd->env, path_native,
                                      (DB_CREATE
                                       | DB_INIT_LOCK
                                       | DB_INIT_LOG
                                       | DB_INIT_MPOOL
                                       | DB_INIT_TXN),
                                      0666));
  if (svn_err) goto error;

  /* Create the databases in the environment.  */
  svn_err = BDB_WRAP (fs, "creating 'nodes' table",
                      svn_fs_bdb__open_nodes_table (&bfd->nodes, bfd->env,
                                                    TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'revisions' table",
                      svn_fs_bdb__open_revisions_table (&bfd->revisions,
                                                        bfd->env, TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'transactions' table",
                      svn_fs_bdb__open_transactions_table (&bfd->transactions,
                                                           bfd->env, TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'copies' table",
                      svn_fs_bdb__open_copies_table (&bfd->copies,
                                                     bfd->env, TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'changes' table",
                      svn_fs_bdb__open_changes_table (&bfd->changes,
                                                      bfd->env, TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'representations' table",
                      svn_fs_bdb__open_reps_table (&bfd->representations,
                                                   bfd->env, TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'strings' table",
                      svn_fs_bdb__open_strings_table (&bfd->strings,
                                                      bfd->env, TRUE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "creating 'uuids' table",
                      svn_fs_bdb__open_uuids_table (&bfd->uuids,
                                                    bfd->env, TRUE));
  if (svn_err) goto error;

  /* Initialize the DAG subsystem. */
  svn_err = svn_fs_base__dag_init_fs (fs);
  if (svn_err) goto error;

  return SVN_NO_ERROR;

error:
  svn_error_clear (cleanup_fs (fs));
  return svn_err;
}


/* Gaining access to an existing Berkeley DB-based filesystem.  */


static svn_error_t *
base_open (svn_fs_t *fs, const char *path, apr_pool_t *pool)
{
  svn_error_t *svn_err;
  const char *path_native;
  base_fs_data_t *bfd;

  SVN_ERR (check_bdb_version (pool));
  SVN_ERR (check_already_open (fs));

  apr_pool_cleanup_register (fs->pool, fs, cleanup_fs_apr,
                             apr_pool_cleanup_null);

  bfd = apr_pcalloc (fs->pool, sizeof (*bfd));
  fs->vtable = &fs_vtable;
  fs->fsap_data = bfd;

  /* Initialize paths. */
  fs->path = apr_pstrdup (fs->pool, path);

  svn_err = allocate_env (fs);
  if (svn_err) goto error;

  /* Open the Berkeley DB environment.  */
  SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, fs->path, fs->pool));
  svn_err = BDB_WRAP (fs, "opening environment",
                      bfd->env->open (bfd->env, path_native,
                                      (DB_CREATE
                                       | DB_INIT_LOCK
                                       | DB_INIT_LOG
                                       | DB_INIT_MPOOL
                                       | DB_INIT_TXN),
                                      0666));
  if (svn_err) goto error;

  /* Open the various databases.  */
  svn_err = BDB_WRAP (fs, "opening 'nodes' table",
                      svn_fs_bdb__open_nodes_table (&bfd->nodes,
                                                    bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'revisions' table",
                      svn_fs_bdb__open_revisions_table (&bfd->revisions,
                                                        bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'transactions' table",
                      svn_fs_bdb__open_transactions_table (&bfd->transactions,
                                                           bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'copies' table",
                      svn_fs_bdb__open_copies_table (&bfd->copies,
                                                     bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'changes' table",
                      svn_fs_bdb__open_changes_table (&bfd->changes,
                                                      bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'representations' table",
                      svn_fs_bdb__open_reps_table (&bfd->representations,
                                                   bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'strings' table",
                      svn_fs_bdb__open_strings_table (&bfd->strings,
                                                      bfd->env, FALSE));
  if (svn_err) goto error;
  svn_err = BDB_WRAP (fs, "opening 'uuids' table",
                      svn_fs_bdb__open_uuids_table (&bfd->uuids,
                                                    bfd->env, FALSE));
  if (svn_err) goto error;

  return SVN_NO_ERROR;

 error:
  svn_error_clear (cleanup_fs (fs));
  return svn_err;
}


/* Running recovery on a Berkeley DB-based filesystem.  */


static svn_error_t *
base_bdb_recover (const char *path,
                  apr_pool_t *pool)
{
  DB_ENV *env;
  const char *path_native;

  SVN_BDB_ERR (db_env_create (&env, 0));

  /* Here's the comment copied from db_recover.c:

     Initialize the environment -- we don't actually do anything
     else, that all that's needed to run recovery.

     Note that we specify a private environment, as we're about to
     create a region, and we don't want to leave it around.  If we
     leave the region around, the application that should create it
     will simply join it instead, and will then be running with
     incorrectly sized (and probably terribly small) caches.  */

  SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, path, pool));
  SVN_BDB_ERR (env->open (env, path_native, (DB_RECOVER | DB_CREATE
                                             | DB_INIT_LOCK | DB_INIT_LOG
                                             | DB_INIT_MPOOL | DB_INIT_TXN
                                             | DB_PRIVATE),
                          0666));
  SVN_BDB_ERR (env->close (env, 0));

  return SVN_NO_ERROR;
}



/* Running the 'archive' command on a Berkeley DB-based filesystem.  */


static svn_error_t *
base_bdb_logfiles (apr_array_header_t **logfiles,
                   const char *path,
                   svn_boolean_t only_unused,
                   apr_pool_t *pool)
{
  DB_ENV *env;
  const char *path_native;
  char **filelist;
  char **filename;
  u_int32_t flags = only_unused ? 0 : DB_ARCH_LOG;

  *logfiles = apr_array_make (pool, 4, sizeof (const char *));

  SVN_BDB_ERR (db_env_create (&env, 0));

  /* Needed on Windows in case Subversion and Berkeley DB are using
     different C runtime libraries  */
  SVN_BDB_ERR (env->set_alloc (env, malloc, realloc, free));

  SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, path, pool));
  SVN_BDB_ERR (env->open (env, path_native, (DB_CREATE
                                             | DB_INIT_LOCK | DB_INIT_LOG
                                             | DB_INIT_MPOOL | DB_INIT_TXN),
                          0666));
  SVN_BDB_ERR (env->log_archive (env, &filelist, flags));

  if (filelist == NULL)
    {
      SVN_BDB_ERR (env->close (env, 0));
      return SVN_NO_ERROR;
    }

  for (filename = filelist; *filename != NULL; ++filename)
    {
      APR_ARRAY_PUSH (*logfiles, const char *) = apr_pstrdup (pool, *filename);
    }

  free (filelist);

  SVN_BDB_ERR (env->close (env, 0));

  return SVN_NO_ERROR;
}



/* Copying a live Berkeley DB-base filesystem.  */

/**
 * Delete all unused log files from DBD enviroment at @a live_path that exist
 * in @a backup_path.
 */
static svn_error_t *
svn_fs_base__clean_logs(const char *live_path,
                        const char *backup_path,
                        apr_pool_t *pool)
{
  apr_array_header_t *logfiles;

  SVN_ERR (base_bdb_logfiles (&logfiles,
                              live_path,
                              TRUE,        /* Only unused logs */
                              pool));

  {  /* Process unused logs from live area */
    int idx;
    apr_pool_t *sub_pool = svn_pool_create (pool);

    /* Process log files. */
    for (idx = 0; idx < logfiles->nelts; idx++)
      {
        const char *log_file = APR_ARRAY_IDX (logfiles, idx, const char *);
        const char *live_log_path;
        const char *backup_log_path;

        svn_pool_clear (sub_pool);
        live_log_path = svn_path_join (live_path, log_file, sub_pool);
        backup_log_path = svn_path_join (backup_path, log_file, sub_pool);

        { /* Compare files. No point in using MD5 and wasting CPU cycles as we
             got full copies of both logs */

          svn_boolean_t files_match = FALSE;
          svn_node_kind_t kind;

          /* Check to see if there is a corresponding log file in the backup
             directory */
          SVN_ERR (svn_io_check_path (backup_log_path, &kind, pool));

          /* If the copy of the log exists, compare them */
          if (kind == svn_node_file)
            SVN_ERR (svn_io_files_contents_same_p (&files_match,
                                                   live_log_path,
                                                   backup_log_path,
                                                   sub_pool));

          /* If log files do not match, go to the next log filr. */
          if (files_match == FALSE)
            continue;
        }

        SVN_ERR (svn_io_remove_file (live_log_path, sub_pool));
      }

    svn_pool_destroy (sub_pool);
  }

  return SVN_NO_ERROR;
}

static svn_error_t *
base_hotcopy (const char *src_path,
              const char *dest_path,
              svn_boolean_t clean_logs,
              apr_pool_t *pool)
{
  /* Check DBD version, just in case */
  SVN_ERR (check_bdb_version (pool));

  /* Copy the DB_CONFIG file. */
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "DB_CONFIG", pool));

  /* Copy the databases.  */
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "nodes", pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "revisions", pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "transactions", pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "copies", pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "changes", pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "representations",
                                 pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "strings", pool));
  SVN_ERR (svn_io_dir_file_copy (src_path, dest_path, "uuids", pool));

  {
    apr_array_header_t *logfiles;
    int idx;

    SVN_ERR (base_bdb_logfiles (&logfiles,
                                src_path,
                                FALSE,   /* All logs */
                                pool));

    /* Process log files. */
    for (idx = 0; idx < logfiles->nelts; idx++)
      {
        SVN_ERR (svn_io_dir_file_copy (src_path, dest_path,
                                       APR_ARRAY_IDX (logfiles, idx,
                                                      const char *),
                                       pool));
      }
  }

  /* Since this is a copy we will have exclusive access to the repository. */
  SVN_ERR (base_bdb_recover (dest_path, pool));

  if (clean_logs == TRUE)
    SVN_ERR (svn_fs_base__clean_logs (src_path, dest_path, pool));

  return SVN_NO_ERROR;
}



/* Deleting a Berkeley DB-based filesystem.  */


static svn_error_t *
base_delete_fs (const char *path,
                apr_pool_t *pool)
{
  DB_ENV *env;
  const char *path_native;

  /* First, use the Berkeley DB library function to remove any shared
     memory segments.  */
  SVN_BDB_ERR (db_env_create (&env, 0));
  SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, path, pool));
  SVN_BDB_ERR (env->remove (env, path_native, DB_FORCE));

  /* Remove the environment directory. */
  SVN_ERR (svn_io_remove_dir (path, pool));

  return SVN_NO_ERROR;
}



/* Base FS library vtable, used by the FS loader library. */

fs_library_vtable_t svn_fs_base__vtable = {
  base_create,
  base_open,
  base_delete_fs,
  base_hotcopy,
  base_bdb_set_errcall,
  base_bdb_recover,
  base_bdb_logfiles,
  svn_fs_base__id_parse
};
