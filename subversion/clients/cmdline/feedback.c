/*
 * feedback.c:  feedback handlers for cmdline client.
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include <stdio.h>
#include <string.h>

#include "svn_pools.h"
#include "cl.h"



/* When the cmd-line client sees an unversioned item during an update,
   print a question mark (`?'), just like CVS does. */
static apr_status_t 
report_unversioned_item (const char *path)
{
  printf ("?  %s\n", path);
              
  return APR_SUCCESS;
}


static apr_status_t 
report_added_item (const char *path, apr_pool_t *pool)
{
  svn_stringbuf_t *spath = svn_stringbuf_create (path, pool);
  svn_wc_entry_t *entry;
  svn_error_t *err;
  int binary = 0;

  err = svn_wc_entry (&entry, spath, pool);
  if (err)
    return err->apr_err;

  if (entry->kind == svn_node_file)
    {
      svn_stringbuf_t *value;
      svn_stringbuf_t *name = svn_stringbuf_create (SVN_PROP_MIME_TYPE, pool);

      err = svn_wc_prop_get (&value, name, spath, pool);
      if (err)
        return err->apr_err;

      /* If the property exists and it doesn't start with `text/', we'll
         call it binary. */
      if ((value) && (value->len > 5) && (strncmp (value->data, "text/", 5)))
        binary = 1;
    }

  printf ("A  %s  %s%s\n",
          binary ? "binary" : "      ",
          path,
          entry->kind == svn_node_dir ? "/" : "");
          
  return APR_SUCCESS;
}


static apr_status_t 
report_unadded_item (const char *path, apr_pool_t *pool)
{
  svn_stringbuf_t *spath = svn_stringbuf_create (path, pool);
  enum svn_node_kind kind;
  svn_error_t *err;

  err = svn_io_check_path (spath, &kind, pool);
  if (err)
    return err->apr_err;

  printf ("-  %s%s\n",
          path,
          kind == svn_node_dir ? "/" : "");
          
  return APR_SUCCESS;
}


static apr_status_t 
report_deleted_item (const char *path, apr_pool_t *pool)
{
  svn_stringbuf_t *spath = svn_stringbuf_create (path, pool);
  svn_wc_entry_t *entry;
  svn_error_t *err;

  err = svn_wc_entry (&entry, spath, pool);
  if (err)
    return err->apr_err;

  printf ("D  %s%s\n",
          path,
          entry->kind == svn_node_dir ? "/" : "");
          
  return APR_SUCCESS;
}


static apr_status_t 
report_undeleted_item (const char *path, apr_pool_t *pool)
{
  svn_stringbuf_t *spath = svn_stringbuf_create (path, pool);
  svn_wc_entry_t *entry;
  svn_error_t *err;

  err = svn_wc_entry (&entry, spath, pool);
  if (err)
    return err->apr_err;

  printf ("+  %s%s\n",
          path,
          entry->kind == svn_node_dir ? "/" : "");
          
  return APR_SUCCESS;
}


static apr_status_t 
report_warning (apr_status_t status, const char *warning)
{
  printf ("WARNING: %s\n", warning);

  /* Someday we can examine STATUS and decide if we should return a
     fatal error. */

  return APR_SUCCESS;
}


#if 0
/* We're not overriding the report_progress feedback vtable function
   at this time. */
static apr_status_t 
report_progress (const char *action, int percentage)
{
  return APR_SUCCESS;
}
#endif


void
svn_cl__init_feedback_vtable (apr_pool_t *top_pool)
{
  svn_pool_feedback_t *feedback_vtable =
    svn_pool_get_feedback_vtable (top_pool);

  feedback_vtable->report_unversioned_item = report_unversioned_item;
  feedback_vtable->report_added_item = report_added_item;
  feedback_vtable->report_unadded_item = report_unadded_item;
  feedback_vtable->report_deleted_item = report_deleted_item;
  feedback_vtable->report_undeleted_item = report_undeleted_item;
  feedback_vtable->report_warning = report_warning;
  /* we're -not- overriding report_progress;  we have no need for it
     yet. */
}



/* 
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end: 
 */
