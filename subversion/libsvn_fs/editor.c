/* editor.c --- a tree editor for commiting changes to a filesystem.
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#include "apr_pools.h"
#include "apr_file_io.h"

#include "svn_error.h"
#include "svn_delta.h"
#include "svn_fs.h"
#include "dag.h"



/*** Editor batons. ***/

struct edit_baton
{
  apr_pool_t *pool;

  /* Subversion file system.
     Supplied by the user when we create the editor.  */
  svn_fs_t *fs;

  /* Existing revision number upon which this edit is based.
     Supplied by the user when we create the editor.  */
  svn_revnum_t base_rev;

  /* Commit message for this commit.
     Supplied by the user when we create the editor.  */
  svn_string_t *log_msg;

  /* Hook to run when when the commit is done. 
     Supplied by the user when we create the editor.  */
  svn_fs_commit_hook_t *hook;
  void *hook_baton;

  /* Transaction associated with this edit.
     This is zero until the driver calls replace_root.  */
  svn_fs_txn_t *txn;

  /* The txn name.  This is just the cached result of applying
     svn_fs_txn_name to TXN, above.
     This is zero until the driver calls replace_root.  */
  char *txn_name;
};


struct dir_baton
{
  struct edit_baton *edit_baton;
  struct dir_baton *parent;
  svn_string_t *name;  /* just this entry, not full path */

  /* This directory, guaranteed to be mutable. */
  dag_node_t *node;

  /* Revision number of this directory */
  svn_revnum_t base_rev;
};


struct file_baton
{
  struct dir_baton *parent;
  svn_string_t *name;  /* just this entry, not full path */

  /* This file, guaranteed to be mutable. */
  dag_node_t *node;

  /* Revision number of this file */
  svn_revnum_t base_rev;
};



/*** Editor functions and their helpers. ***/

/* Helper for replace_root. */
static svn_error_t *
txn_body_clone_root (void *dir_baton, trail_t *trail)
{
  struct dir_baton *dirb = dir_baton;

  SVN_ERR (svn_fs__dag_clone_root (&(dirb->node),
                                   dirb->edit_baton->fs,
                                   dirb->edit_baton->txn_name,
                                   trail));

  return SVN_NO_ERROR;
}


static svn_error_t *
replace_root (void *edit_baton, svn_revnum_t base_revision, void **root_baton)
{
  /* kff todo: figure out breaking into subpools soon */
  struct edit_baton *eb = edit_baton;
  struct dir_baton *dirb = apr_pcalloc (eb->pool, sizeof (*dirb));

  /* Begin a transaction. */
  SVN_ERR (svn_fs_begin_txn (&(eb->txn), eb->fs, eb->base_rev, eb->pool));

  /* Cache the transaction's name. */
  SVN_ERR (svn_fs_txn_name (&(eb->txn_name), eb->txn, eb->pool));
  
  /* What don't we do?
   * 
   * What we don't do is start a single Berkeley DB transaction here,
   * keep it open throughout the entire edit, and then call
   * txn_commit() inside close_edit().  That would result in writers
   * interfering with writers unnecessarily.
   * 
   * Instead, we take small steps.  When we clone the root node, it
   * actually gets a new node -- a mutable one -- in the nodes table.
   * If we clone the next dir down, it gets a new node then too.  When
   * it's time to commit, we'll walk those nodes (it doesn't matter in
   * what order), looking for irreconcilable conflicts but otherwise
   * merging changes from immutable dir nodes into our mutable ones.
   *
   * When our private tree is all in order, we lock a revision and
   * walk again, making sure the final merge states are sane.  Then we
   * mark them all as immutable and hook in the new root.
   */

  /* Set up the root directory baton, the last step of which is to get
     a new root directory for this txn, cloned from the root dir of
     the txn's base revision. */
  dirb->edit_baton = edit_baton;
  dirb->parent = NULL;
  dirb->name = svn_string_create ("", eb->pool);
  dirb->base_rev = eb->base_rev;
  SVN_ERR (svn_fs__retry_txn (eb->fs, txn_body_clone_root, dirb, eb->pool));

  /* kff todo: If there was any error, this transaction will have to
     be cleaned up, including removing its nodes from the nodes
     table. */

  *root_baton = dirb;
  return SVN_NO_ERROR;
}


/* Helper for delete_node and delete_entry. */
struct delete_args
{
  struct dir_baton *parent;
  svn_string_t *name;
};


/* Helper for delete_entry. */
static svn_error_t *
txn_body_delete (void *del_baton, trail_t *trail)
{
  struct delete_args *del_args = del_baton;

  SVN_ERR (svn_fs__dag_delete (del_args->parent->node,
                               del_args->name->data,
                               trail));

  return SVN_NO_ERROR;
}


static svn_error_t *
delete_entry (svn_string_t *name, void *parent_baton)
{
  struct dir_baton *dirb = parent_baton;
  struct edit_baton *eb = dirb->edit_baton;
  struct delete_args del_args;
  
  del_args.parent = dirb;
  del_args.name   = name;
  
  SVN_ERR (svn_fs__retry_txn (eb->fs, txn_body_delete, &del_args, eb->pool));

  return SVN_NO_ERROR;
}


/* Helper for addition and replacement of files and directories. */
struct add_repl_args
{
  struct dir_baton *parent;  /* parent in which we're adding|replacing */
  svn_string_t *name;        /* name of what we're adding|replacing */
  dag_node_t *new_node;      /* what we added|replaced */
};


/* Helper for add_directory. */
static svn_error_t *
txn_body_add_directory (void *add_baton, trail_t *trail)
{
  struct add_repl_args *add_args = add_baton;
  
  SVN_ERR (svn_fs__dag_make_dir (&(add_args->new_node),
                                 add_args->parent->node,
                                 add_args->name->data,
                                 trail));

  return SVN_NO_ERROR;
}


static svn_error_t *
add_directory (svn_string_t *name,
               void *parent_baton,
               svn_string_t *ancestor_path,
               long int ancestor_revision,
               void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *new_dirb
    = apr_pcalloc (pb->edit_baton->pool, sizeof (*new_dirb));
  struct add_repl_args add_args;
  
  add_args.parent = pb;
  add_args.name = name;
  
  SVN_ERR (svn_fs__retry_txn (pb->edit_baton->fs,
                              txn_body_add_directory,
                              &add_args,
                              pb->edit_baton->pool));

  new_dirb->edit_baton = pb->edit_baton;
  new_dirb->parent = pb;
  new_dirb->name = svn_string_dup (name, pb->edit_baton->pool);
  new_dirb->node = add_args.new_node;

  *child_baton = new_dirb;
  return SVN_NO_ERROR;
}


static svn_error_t *
txn_body_replace_directory (void *rargs, trail_t *trail)
{
  struct add_repl_args *repl_args = rargs;

  SVN_ERR (svn_fs__dag_clone_child (&(repl_args->new_node),
                                    repl_args->parent->node,
                                    repl_args->name->data,
                                    trail));

  if (! svn_fs__dag_is_directory (repl_args->new_node))
    {
      return svn_error_createf (SVN_ERR_FS_NOT_DIRECTORY,
                                0,
                                NULL,
                                trail->pool,
                                "trying to replace directory, but %s "
                                "is not a directory",
                                repl_args->name->data);
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
replace_directory (svn_string_t *name,
                   void *parent_baton,
                   svn_revnum_t base_revision,
                   void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *dirb = apr_pcalloc (pb->edit_baton->pool, sizeof (*dirb));
  struct add_repl_args repl_args;
  
  repl_args.parent = pb;
  repl_args.name   = name;

  SVN_ERR (svn_fs__retry_txn (pb->edit_baton->fs,
                              txn_body_replace_directory,
                              &repl_args,
                              pb->edit_baton->pool));

  dirb->edit_baton = pb->edit_baton;
  dirb->parent = pb;
  dirb->name = svn_string_dup (name, pb->edit_baton->pool);
  dirb->node = repl_args.new_node;

  *child_baton = dirb;
  return SVN_NO_ERROR;
}


static svn_error_t *
close_directory (void *dir_baton)
{
  /* One might be tempted to make this function mark the directory as
     immutable; that way, if the traversal order is violated somehow,
     we'll get an error the second time we visit the directory.

     However, that would be incorrect --- the node must remain
     mutable, since we may have to merge changes into it before we can
     commit the transaction.  */

  return SVN_NO_ERROR;
}


static svn_error_t *
close_file (void *file_baton)
{
  /* This function could mark the file as immutable, since even the
     final pre-commit merge doesn't touch file contents.  (See the
     comment above in `close_directory'.)  */
  return SVN_NO_ERROR;
}


static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *handler_pair)
{
  return SVN_NO_ERROR;
}


static svn_error_t *
apply_textdelta (void *file_baton,
                 svn_txdelta_window_handler_t **handler,
                 void **handler_baton)
{
  *handler = window_handler;
  *handler_baton = file_baton;
  return SVN_NO_ERROR;
}


/* Helper for add_file. */
static svn_error_t *
txn_body_add_file (void *add_baton, trail_t *trail)
{
  struct add_repl_args *add_args = add_baton;

  SVN_ERR (svn_fs__dag_make_file (&(add_args->new_node),
                                  add_args->parent->node,
                                  add_args->name->data,
                                  trail));

  return SVN_NO_ERROR;
}


static svn_error_t *
add_file (svn_string_t *name,
          void *parent_baton,
          svn_string_t *ancestor_path,
          long int ancestor_revision,
          void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *new_fb
    = apr_pcalloc (pb->edit_baton->pool, sizeof (*new_fb));
  struct add_repl_args add_args;

  add_args.parent = pb;
  add_args.name = name;

  SVN_ERR (svn_fs__retry_txn (pb->edit_baton->fs,
                              txn_body_add_file,
                              &add_args,
                              pb->edit_baton->pool));

  new_fb->parent = pb;
  new_fb->name = svn_string_dup (name, pb->edit_baton->pool);
  new_fb->node = add_args.new_node;

  *file_baton = new_fb;
  return SVN_NO_ERROR;
}


static svn_error_t *
txn_body_replace_file (void *rargs, trail_t *trail)
{
  struct add_repl_args *repl_args = rargs;

  SVN_ERR (svn_fs__dag_clone_child (&(repl_args->new_node),
                                    repl_args->parent->node,
                                    repl_args->name->data,
                                    trail));
  
  if (! svn_fs__dag_is_file (repl_args->new_node))
    {
      return svn_error_createf (SVN_ERR_FS_NOT_DIRECTORY,
                                0,
                                NULL,
                                trail->pool,
                                "trying to replace directory, but %s "
                                "is not a directory",
                                repl_args->name->data);
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
replace_file (svn_string_t *name,
              void *parent_baton,
              svn_revnum_t base_revision,
              void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *fb = apr_pcalloc (pb->edit_baton->pool, sizeof (*fb));
  struct add_repl_args repl_args;

  repl_args.parent = pb;
  repl_args.name   = name;

  SVN_ERR (svn_fs__retry_txn (pb->edit_baton->fs, txn_body_replace_file,
                              &repl_args, pb->edit_baton->pool));

  fb->parent = pb;
  fb->name = svn_string_dup (name, pb->edit_baton->pool);
  fb->node = repl_args.new_node;

  *file_baton = fb;
  return SVN_NO_ERROR;
}


static svn_error_t *
change_file_prop (void *file_baton,
                  svn_string_t *name,
                  svn_string_t *value)
{
  return SVN_NO_ERROR;
}


static svn_error_t *
change_dir_prop (void *dir_baton,
                 svn_string_t *name,
                 svn_string_t *value)
{
  return SVN_NO_ERROR;
}


static svn_error_t *
close_edit (void *edit_baton)
{
  struct edit_baton *eb = edit_baton;
  svn_revnum_t new_revision = SVN_INVALID_REVNUM;

  SVN_ERR (svn_fs_commit_txn (&new_revision, eb->txn));
  SVN_ERR ((*eb->hook) (new_revision, eb->hook_baton));

  return SVN_NO_ERROR;
}



/*** Public interface. ***/

svn_error_t *
svn_fs_get_editor (svn_delta_edit_fns_t **editor,
                   void **edit_baton,
                   svn_fs_t *fs,
                   svn_revnum_t base_revision,
                   svn_string_t *log_msg,
                   svn_fs_commit_hook_t *hook,
                   void *hook_baton,
                   apr_pool_t *pool)
{
  svn_delta_edit_fns_t *e = svn_delta_default_editor (pool);
  apr_pool_t *subpool = svn_pool_create (pool);
  struct edit_baton *eb = apr_pcalloc (subpool, sizeof (*eb));

  /* Set up the editor. */
  e->replace_root      = replace_root;
  e->delete_entry      = delete_entry;
  e->add_directory     = add_directory;
  e->replace_directory = replace_directory;
  e->change_dir_prop   = change_dir_prop;
  e->close_directory   = close_directory;
  e->add_file          = add_file;
  e->replace_file      = replace_file;
  e->apply_textdelta   = apply_textdelta;
  e->change_file_prop  = change_file_prop;
  e->close_file        = close_file;
  e->close_edit        = close_edit;

  /* Set up the edit baton. */
  eb->pool = subpool;
  eb->log_msg = svn_string_dup (log_msg, subpool);
  eb->hook = hook;
  eb->hook_baton = hook_baton;
  eb->base_rev = base_revision;

  *edit_baton = eb;
  *editor = e;
  
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
