/*
 * get_editor.c :  routines for update and checkout
 *
 * ================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by CollabNet (http://www.Collab.Net)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of CollabNet.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLABNET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by many
 * individuals on behalf of CollabNet.
 */



#include <stdio.h>       /* temporary, for printf() */
#include <stdlib.h>
#include <string.h>
#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_file_io.h>
#include <apr_strings.h>
#include "svn_types.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_hash.h"
#include "svn_wc.h"

#include "wc.h"



/*** batons ***/

struct edit_baton
{
  svn_string_t *dest_dir;
  svn_string_t *repository;
  svn_vernum_t version;
  apr_pool_t *pool;
};


struct dir_baton
{
  /* The path to this directory. */
  svn_string_t *path;

  /* The number of other changes associated with this directory in the
     delta (typically, the number of files being changed here, plus
     this dir itself).  BATON->ref_count starts at 1, is incremented
     for each entity being changed, and decremented for each
     completion of one entity's changes.  When the ref_count is 0, the
     directory may be safely set to the target version, and this baton
     freed. */
  int ref_count;

  /* The global edit baton. */
  /* kff todo: suspect we may never use this, remove it if so. */
  struct edit_baton *edit_baton;

  /* The pool in which this baton itself is allocated. */
  apr_pool_t *pool;
};


/* Create a new dir_baton for subdir NAME in PARENT_PATH with
 * EDIT_BATON, using a new subpool of POOL.
 * The new baton's ref_count is 1.
 */
static struct dir_baton *
make_dir_baton (svn_string_t *parent_path,
                svn_string_t *name,
                struct edit_baton *edit_baton,
                apr_pool_t *pool)
{
  apr_pool_t *subpool = apr_make_sub_pool (pool, NULL);
  struct dir_baton *d = apr_pcalloc (subpool, sizeof (*d));

  svn_string_t *path = svn_string_dup (parent_path, subpool);

  /* The initial baton usually takes no name. */
  if (name)
    svn_string_appendstr (path, name, subpool);

  d->path       = path;
  d->edit_baton = edit_baton;
  d->ref_count  = 1;
  d->pool       = subpool;

  return d;
}


static svn_error_t *
free_dir_baton (struct dir_baton *dir_baton)
{
  /* Do whatever cleanup is needed on PATH with EDIT_BATON. */

  /* kff todo fooo working here */

  /* After we destroy DIR_BATON->pool, DIR_BATON itself is lost. */
  apr_destroy_pool (dir_baton->pool);
}


/* Decrement DIR_BATON's ref count, and if the count hits 0, do the
 * appropriate cleanups.
 *
 * Note: There is no corresponding function for incrementing the
 * ref_count.  As far as we know, nothing special depends on that, so
 * it's just done inline.
 */
static svn_error_t *
decrement_ref_count (struct dir_baton *d)
{
  d->ref_count--;

  if (d->ref_count == 0)
    return free_dir_baton (d);

  return SVN_NO_ERROR;
}


struct file_baton
{
  struct dir_baton *dir_baton;  /* parent dir's baton */
  svn_string_t *path;           /* full (abs or relative) path to the file */
};


/* NAME is just one component, not a path. */
static struct file_baton *
make_file_baton (struct dir_baton *parent_dir_baton, svn_string_t *name)
{
  struct file_baton *f = apr_pcalloc (parent_dir_baton->pool, sizeof (*f));
  svn_string_t *path = svn_string_dup (parent_dir_baton->path,
                                       parent_dir_baton->pool);

  svn_string_appendstr (path, name, parent_dir_baton->pool);

  f->dir_baton  = parent_dir_baton;
  f->path       = path;

  return f;
}



/*** Helpers for the editor callbacks. ***/

static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  int i;
  struct file_baton *fb = (struct file_baton *) baton;
  apr_file_t *dest = NULL;
  apr_status_t apr_err;

  /* kff todo: get more sophisticated when we can handle more ops. */
  apr_err = apr_open (&dest, fb->path->data,
                      (APR_WRITE | APR_APPEND | APR_CREATE),
                      APR_OS_DEFAULT,
                      window->pool);
  if (apr_err)
    return svn_create_error (apr_err, 0, fb->path->data, NULL, window->pool);
  
  /* else */

  for (i = 0; i < window->num_ops; i++)
    {
      svn_txdelta_op_t this_op = (window->ops)[i];
      switch (this_op.action_code)
        {
        case svn_txdelta_source:
          /* todo */
          break;

        case svn_txdelta_target:
          /* todo */
          break;

        case svn_txdelta_new:
          {
            apr_status_t apr_err;
            apr_size_t written;
            const char *data = ((svn_string_t *) (window->new))->data;

            printf ("%.*s", (int) this_op.length, (data + this_op.offset));
            apr_err = apr_full_write (dest, (data + this_op.offset),
                                      this_op.length, &written);
            if (apr_err)
              return svn_create_error (apr_err, 0, NULL, NULL, window->pool);

            break;
          }
        }
    }

  apr_err = apr_close (dest);
  if (apr_err)
    return svn_create_error (apr_err, 0, fb->path->data, NULL, window->pool);

  /* else */

  return SVN_NO_ERROR;
}



/*** The callbacks we'll plug into an svn_delta_edit_fns_t structure. ***/

static svn_error_t *
delete (svn_string_t *name, void *edit_baton, void *parent_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;

  /* kff todo */

  return SVN_NO_ERROR;
}


static svn_error_t *
add_directory (svn_string_t *name,
               void *edit_baton,
               void *parent_baton,
               svn_string_t *ancestor_path,
               svn_vernum_t ancestor_version,
               void **child_baton)
{
  svn_error_t *err;
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;

  struct dir_baton *this_dir_baton
    = make_dir_baton (parent_dir_baton->path,
                      name,
                      eb,
                      apr_make_sub_pool (parent_dir_baton->pool, NULL));

  err = svn_wc__ensure_wc (parent_dir_baton->path, eb->repository, eb->pool);
  if (err)
    return err;

  /* kff todo urgent: at some point in here we need to let the parent
     know this new subdirectory exists! */

  /* kff todo: how about a sanity check that it's not a dir of the
     same name from a different repository or something? 
     Well, that will be later on down the line... */

  /* Make sure the directory exists. */
  err = svn_wc__ensure_directory (this_dir_baton->path, eb->pool);
  if (err)
    return err;

  /* Make sure it's a working copy. */
  err = svn_wc__ensure_wc (this_dir_baton->path, eb->repository, eb->pool);
  if (err)
    return err;

  printf ("%s\n", this_dir_baton->path->data);

#if 0
  /* kff todo: fooo working here.
     Setup has to be done carefully.  We have set up the directory
     NAME, but also let PATH know about it iff PATH is a concerned
     working copy. */
  err = svn_wc__blah_blah_blah (npath,
                                ancestor_path,
                                ancestor_version,
                                eb->pool);
  if (err)
    return err;
#endif /* 0 */

  /* else */

  *child_baton = this_dir_baton;
  return SVN_NO_ERROR;
}


static svn_error_t *
replace_directory (svn_string_t *name,
                   void *edit_baton,
                   void *parent_baton,
                   svn_string_t *ancestor_path,
                   svn_vernum_t ancestor_version,
                   void **child_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;

  /* kff todo */

  return SVN_NO_ERROR;
}


static svn_error_t *
change_dir_prop (void *edit_baton,
                 void *dir_baton,
                 svn_string_t *name,
                 svn_string_t *value)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *this_dir_baton = (struct dir_baton *) dir_baton;

  /* kff todo */
  return SVN_NO_ERROR;
}


static svn_error_t *
change_dirent_prop (void *edit_baton,
                    void *dir_baton,
                    svn_string_t *entry,
                    svn_string_t *name,
                    svn_string_t *value)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *this_dir_baton = (struct dir_baton *) dir_baton;

  /* kff todo */
  return SVN_NO_ERROR;
}


static svn_error_t *
finish_directory (void *edit_baton, void *dir_baton)
{
  svn_error_t *err = NULL;
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *this_dir_baton = (struct dir_baton *) dir_baton;

  err = decrement_ref_count (dir_baton);

  /* kff todo: now that the child is finished, we should make an entry
     in the parent's base-tree (although frankly I'm beginning to
     wonder if child directories should be recorded anywhere but in
     themselves; perhaps that would be best, and just let the parent
     deduce their existence.  We can still tell when an update of the
     parent is complete, by refcounting.) */

  return err;
}


static svn_error_t *
add_file (svn_string_t *name,
          void *edit_baton,
          void *parent_baton,
          svn_string_t *ancestor_path,
          svn_vernum_t ancestor_version,
          void **file_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;
  struct file_baton *fb;
  svn_error_t *err;

  /* kff todo fooo: this should go away once we can guarantee a call
     either to {add,replace}_directory() or start_edit() before the
     first add_file(). */
  {
    /* fooo: nothing can happen until the first dir_baton is taken
       care of, fix callers to make this happen */
  }

  /* Make sure we've got a working copy to put the file in. */
  err = svn_wc__check_wc (parent_dir_baton->path, parent_dir_baton->pool);
  if (err)
    return err;

  /* Okay, looks like we're good to go. */

  /* kff todo urgent: check all calls you know what I mean. */
  fb = make_file_baton (parent_dir_baton, name);
  parent_dir_baton->ref_count++;
  *file_baton = fb;

  printf ("%s\n   ", fb->path);

  return SVN_NO_ERROR;
}


static svn_error_t *
replace_file (svn_string_t *name,
              void *edit_baton,
              void *parent_baton,
              svn_string_t *ancestor_path,
              svn_vernum_t ancestor_version,
              void **file_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;
  svn_error_t *err = NULL;

  /* Replacing is mostly like adding... */
  err = add_file (name,
                  eb,
                  parent_dir_baton,
                  ancestor_path,
                  ancestor_version, 
                  file_baton);

  /* ... except that you must check that the file existed already, and
     was under version control */

  /* kff todo urgent: HERE, do what above says */

  printf ("replace file \"%s\"\n", *file_baton);

  return err;
}


static svn_error_t *
apply_textdelta (void *edit_baton,
                 void *parent_baton,
                 void *file_baton, 
                 svn_txdelta_window_handler_t **handler,
                 void **handler_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;

  /* kff todo: dance the tmp file dance, eventually. */
  
  *handler_baton = file_baton;
  *handler = window_handler;
  
  return SVN_NO_ERROR;
}


static svn_error_t *
change_file_prop (void *edit_baton,
                  void *parent_baton,
                  void *file_baton,
                  svn_string_t *name,
                  svn_string_t *value)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *parent_dir_baton = (struct dir_baton *) parent_baton;
  struct file_baton *fb = (struct file_baton *) file_baton;

  /* kff todo */

  return SVN_NO_ERROR;
}


static svn_error_t *
finish_file (void *edit_baton, void *file_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct file_baton *fb = (struct file_baton *) file_baton;
  svn_error_t *err;

  err = svn_wc__lock (fb->dir_baton->path, 0, fb->dir_baton->pool);
  if (err)
    return err;

  /* kff todo urgent: NOW, write the log and then do what it promises. */

  err = svn_wc__unlock (fb->dir_baton->path, fb->dir_baton->pool);
  if (err)
    return err;

  printf ("\n");
  return SVN_NO_ERROR;
}


static svn_error_t *
finish_edit (void *edit_baton, void *dir_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  struct dir_baton *this_dir_baton = (struct dir_baton *) dir_baton;
  svn_error_t *err;
  int stack_empty;

  /* The edit is over, free its pool. */
  apr_destroy_pool (eb->pool);

  /* kff todo:  Wow.  Is there _anything_ else that needs to be done? */

  printf ("\n");
  return SVN_NO_ERROR;
}



static const svn_delta_edit_fns_t tree_editor =
{
  delete,
  add_directory,
  replace_directory,
  change_dir_prop,
  change_dirent_prop,
  finish_directory,
  add_file,
  replace_file,
  apply_textdelta,
  change_file_prop,
  finish_file,
  finish_edit
};


svn_error_t *
svn_wc_get_update_editor (svn_string_t *dest,
                          svn_string_t *repos,
                          svn_vernum_t version,
                          const svn_delta_edit_fns_t **editor,
                          void **edit_baton,
                          void **dir_baton,
                          apr_pool_t *pool)
{
  svn_error_t *err;
  struct edit_baton *eb;
  apr_pool_t *subpool;

  subpool = apr_make_sub_pool (pool, NULL);

  /* Else nothing in the way, so continue. */

  *editor = &tree_editor;

  eb = apr_pcalloc (subpool, sizeof (*edit_baton));
  eb->dest_dir   = dest;   /* Remember, DEST might be null. */
  eb->repository = repos;
  eb->pool       = subpool;
  eb->version    = version;

  *edit_baton = eb;

  *dir_baton = make_dir_baton (dest, NULL, eb, subpool);

  return NULL;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
