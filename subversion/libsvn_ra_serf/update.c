/*
 * update.c :  entry point for update RA functions for ra_serf
 *
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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



#define APR_WANT_STRFUNC
#include <apr_want.h>

#include <apr_uri.h>

#include <expat.h>

#include <serf.h>

#include "svn_pools.h"
#include "svn_ra.h"
#include "svn_dav.h"
#include "svn_xml.h"
#include "../libsvn_ra/ra_loader.h"
#include "svn_config.h"
#include "svn_delta.h"
#include "svn_version.h"
#include "svn_path.h"
#include "svn_private_config.h"

#include "ra_serf.h"


/*
 * This enum represents the current state of our XML parsing for a REPORT.
 */
typedef enum {
    OPEN_DIR,
    ADD_DIR,
    OPEN_FILE,
    ADD_FILE,
    PROP,
    IGNORE_PROP_NAME,
    NEED_PROP_NAME,
} report_state_e;

/*
 * This structure represents the information for a directory.
 */
typedef struct report_dir_t
{
  /* Our parent directory.
   *
   * This value is NULL when we are the root.
   */
  struct report_dir_t *parent_dir;

  apr_pool_t *pool;

  /* Our name sans any parents. */
  const char *base_name;

  /* the expanded directory name (including all parent names) */
  const char *name;

  /* temporary path buffer for this directory. */
  svn_stringbuf_t *name_buf;

  /* the canonical url for this directory. */
  const char *url;

  /* Our base revision - SVN_INVALID_REVNUM if we're adding this dir. */
  svn_revnum_t base_rev;

  /* controlling dir baton - this is only created in open_dir() */
  void *dir_baton;
  apr_pool_t *dir_baton_pool;

  /* Our master update editor and baton. */
  const svn_delta_editor_t *update_editor;
  void *update_baton;

  /* How many references to this directory do we still have open? */
  apr_size_t ref_count;

  /* Namespace list allocated out of this ->pool. */
  ns_t *ns_list;

  /* hashtable that stores all of the properties (shared with a dir) */
  apr_hash_t *props;

  /* The propfind request for our current directory */
  propfind_context_t *propfind;

  /* The children of this directory  */
  struct report_dir_t *children;

  /* The next sibling of this directory */
  struct report_dir_t *sibling;
} report_dir_t;

/*
 * This structure represents the information for a file.
 *
 * A directory may have a report_info_t associated with it as well.
 *
 * This structure is created as we parse the REPORT response and
 * once the element is completed, we create a report_fetch_t structure
 * to give to serf to retrieve this file.
 */
typedef struct report_info_t
{
  apr_pool_t *pool;

  /* The enclosing directory.
   *
   * If this structure refers to a directory, the dir it points to will be
   * itself.
   */
  report_dir_t *dir;

  /* Our name sans any directory info. */
  const char *base_name;

  /* the expanded file name (including all parent directory names) */
  const char *name;

  /* file name buffer */
  svn_stringbuf_t *name_buf;

  /* the canonical url for this file. */
  const char *url;

  /* Our base revision - SVN_INVALID_REVNUM if we're adding this file. */
  svn_revnum_t base_rev;

  /* our delta base, if present (NULL if we're adding the file) */
  const svn_string_t *delta_base;

  /* pool passed to update->add_file, etc. */
  apr_pool_t *editor_pool;

  /* controlling file_baton and textdelta handler */
  void *file_baton;
  svn_txdelta_window_handler_t textdelta;
  void *textdelta_baton;

  /* temporary property for this file which is currently being parsed
   * It will eventually be stored in our parent directory's property hash.
   */
  const char *prop_ns;
  const char *prop_name;
  const char *prop_val;
  apr_size_t prop_val_len;
} report_info_t;

/*
 * This structure represents a single request to GET (fetch) a file with
 * its associated Serf session/connection.
 */
typedef struct report_fetch_t {
  /* Our pool. */
  apr_pool_t *pool;

  /* The session we should use to fetch the file. */
  ra_serf_session_t *sess;

  /* The connection we should use to fetch file. */
  ra_serf_connection_t *conn;

  /* Stores the information for the file we want to fetch. */
  report_info_t *info;

  /* Have we read our response headers yet? */
  svn_boolean_t read_headers;

  /* This flag is set when our response is aborted before we reach the
   * end and we decide to requeue this request.
   */
  svn_boolean_t aborted_read;
  apr_off_t aborted_read_size;

  /* This is the amount of data that we have read so far. */
  apr_off_t read_size;

  /* If we're receiving an svndiff, this will be non-NULL. */
  svn_stream_t *delta_stream;

  /* Are we done fetching this file? */
  ra_serf_list_t **done_list;
  ra_serf_list_t done_item;

} report_fetch_t;

/*
 * Encapsulates all of the REPORT parsing state that we need to know at
 * any given time.
 *
 * Previous states are stored in ->prev field.
 */
typedef struct report_state_list_t {
   /* The current state that we are in now. */
  report_state_e state;

  /* Information */
  report_info_t *info;

  /* Temporary pool */
  apr_pool_t *pool;

  /* Temporary namespace list allocated from ->pool */
  ns_t *ns_list;

  /* The previous state we were in. */
  struct report_state_list_t *prev;
} report_state_list_t;

/*
 * The master structure for a REPORT request and response.
 */
typedef struct {
  apr_pool_t *pool;

  ra_serf_session_t *sess;
  ra_serf_connection_t *conn;

  /* What is the target revision that we want for this REPORT? */
  const char *target;
  svn_revnum_t target_rev;

  svn_boolean_t recurse;

  /* Our master update editor and baton. */
  const svn_delta_editor_t *update_editor;
  void *update_baton;

  /* The request body for the REPORT. */
  serf_bucket_t *buckets;

  /* Our XML parser and root namespace for parsing the response. */
  XML_Parser xmlp;
  ns_t *ns_list;

  /* the current state we are in for parsing the REPORT response.
   *
   * could allocate this as an array rather than a linked list.
   *
   * (We tend to use only about 8 or 9 states in a given update-report,
   * but in theory it could be much larger based on the number of directories
   * we are adding.)
   */
  report_state_list_t *state;
  /* A list of previous states that we have created but aren't using now. */
  report_state_list_t *free_state;

  /* root directory object */
  report_dir_t *root_dir;

  /* number of pending GET requests */
  unsigned int active_fetches;

  /* completed fetches (contains report_fetch_t) */
  ra_serf_list_t *done_fetches;

  /* number of pending PROPFIND requests */
  unsigned int active_propfinds;

  /* completed PROPFIND requests (contains propfind_context_t) */
  ra_serf_list_t *done_propfinds;

  /* free list of info structures */
  report_info_t *free_info;

  /* The path to the REPORT request */
  const char *path;

  /* Are we done parsing the REPORT response? */
  svn_boolean_t done;

} report_context_t;


static void push_state(report_context_t *ctx, report_state_e state)
{
  report_state_list_t *new_state;

  if (!ctx->free_state)
    {
      new_state = apr_palloc(ctx->sess->pool, sizeof(*ctx->state));

      apr_pool_create(&new_state->pool, ctx->sess->pool);
    }
  else
    {
      new_state = ctx->free_state;
      ctx->free_state = ctx->free_state->prev;

      apr_pool_clear(new_state->pool);
    }
  new_state->state = state;

  if (!ctx->state && state == OPEN_DIR)
    {
      new_state->info = apr_palloc(ctx->sess->pool, sizeof(*new_state->info));
      apr_pool_create(&new_state->info->pool, ctx->sess->pool);

      /* Create our root state now. */
      new_state->info->dir = apr_pcalloc(ctx->sess->pool,
                                        sizeof(*new_state->info->dir));
      new_state->info->dir->pool = new_state->info->pool;

      new_state->info->dir->propfind = NULL;

      /* Create the root property tree. */
      new_state->info->dir->props = apr_hash_make(new_state->info->pool);
      new_state->info->dir->ns_list = NULL;

      /* Point to the update_editor */
      new_state->info->dir->update_editor = ctx->update_editor;
      new_state->info->dir->update_baton = ctx->update_baton;

      /* Allow us to be found later. */
      ctx->root_dir = new_state->info->dir;
    }
  else if (state == ADD_DIR || state == OPEN_DIR)
    {
      new_state->info = apr_palloc(ctx->state->info->pool,
                                   sizeof(*new_state->info));
      apr_pool_create(&new_state->info->pool, ctx->state->info->pool);

      new_state->info->dir =
          apr_pcalloc(ctx->state->info->pool, sizeof(*new_state->info->dir));
      new_state->info->dir->pool = new_state->info->pool;
      new_state->info->dir->parent_dir = ctx->state->info->dir;
      new_state->info->dir->parent_dir->ref_count++;

      new_state->info->dir->propfind = NULL;

      new_state->info->dir->props = apr_hash_make(new_state->info->pool);

      /* Point our ns_list at our parents to try to reuse it. */
      new_state->info->dir->ns_list =
          new_state->info->dir->parent_dir->ns_list;

      /* Point to the update_editor */
      new_state->info->dir->update_editor = ctx->update_editor;
      new_state->info->dir->update_baton = ctx->update_baton;

      /* Add ourselves to our parent's list */
      new_state->info->dir->sibling = ctx->state->info->dir->children;
      ctx->state->info->dir->children = new_state->info->dir;
    }
  else if (state == OPEN_FILE || state == ADD_FILE)
    {
      new_state->info = apr_palloc(ctx->state->info->pool,
                                   sizeof(*new_state->info));
      apr_pool_create(&new_state->info->pool, ctx->state->info->pool);
      new_state->info->file_baton = NULL;

      /* Point at our parent's directory state. */
      new_state->info->dir = ctx->state->info->dir;
      new_state->info->dir->ref_count++;
    }
  /* if we have state info from our parent, reuse it. */
  else if (ctx->state && ctx->state->info)
    {
      new_state->info = ctx->state->info;
    }
  else
    {
      abort();
    }

  if (!ctx->state)
    {
      /* Attach to the root state. */
      new_state->ns_list = ctx->ns_list;
    }
  else
    {
      new_state->ns_list = ctx->state->ns_list;
    }

  /* Add it to the state chain. */
  new_state->prev = ctx->state;
  ctx->state = new_state;
}

static void pop_state(report_context_t *ctx)
{
  report_state_list_t *free_state;
  free_state = ctx->state;
  /* advance the current state */
  ctx->state = ctx->state->prev;
  free_state->prev = ctx->free_state;
  ctx->free_state = free_state;
  ctx->free_state->info = NULL;
}

typedef svn_error_t * (*prop_set_t)(void *baton,
                                    const char *name,
                                    const svn_string_t *value,
                                    apr_pool_t *pool);

static void
set_baton_props(prop_set_t setprop, void *baton,
                const void *ns, apr_ssize_t ns_len,
                const void *name, apr_ssize_t name_len,
                void *val,
                apr_pool_t *pool)
{
  const char *prop_name;
  svn_string_t *prop_val;

  if (strcmp(ns, SVN_DAV_PROP_NS_CUSTOM) == 0)
    prop_name = name;
  else if (strcmp(ns, SVN_DAV_PROP_NS_SVN) == 0)
    prop_name = apr_pstrcat(pool, SVN_PROP_PREFIX, name, NULL);
  else if (strcmp(name, "version-name") == 0)
    prop_name = SVN_PROP_ENTRY_COMMITTED_REV;
  else if (strcmp(name, "creationdate") == 0)
    prop_name = SVN_PROP_ENTRY_COMMITTED_DATE;
  else if (strcmp(name, "creator-displayname") == 0)
    prop_name = SVN_PROP_ENTRY_LAST_AUTHOR;
  else if (strcmp(name, "repository-uuid") == 0)
    prop_name = SVN_PROP_ENTRY_UUID;
  else if (strcmp(name, "checked-in") == 0)
    prop_name = RA_SERF_WC_CHECKED_IN_URL;
  else
    {
      /* do nothing for now? */
      return;
    }

  setprop(baton, prop_name, svn_string_create(val, pool), pool);
}

static void
set_file_props(void *baton,
               const void *ns, apr_ssize_t ns_len,
               const void *name, apr_ssize_t name_len,
               void *val,
               apr_pool_t *pool)
{
  report_info_t *info = baton;
  set_baton_props(info->dir->update_editor->change_file_prop,
                  info->file_baton,
                  ns, ns_len, name, name_len, val, pool);
}

static void
set_dir_props(void *baton,
              const void *ns, apr_ssize_t ns_len,
              const void *name, apr_ssize_t name_len,
              void *val,
              apr_pool_t *pool)
{
  report_dir_t *dir = baton;
  set_baton_props(dir->update_editor->change_dir_prop,
                  dir->dir_baton,
                  ns, ns_len, name, name_len, val, pool);
}

static svn_error_t*
open_dir(report_dir_t *dir)
{
  /* if we're already open, return now */
  if (dir->dir_baton)
    {
      return SVN_NO_ERROR;
    }

  if (dir->base_name[0] == '\0')
    {
      apr_pool_create(&dir->dir_baton_pool, dir->pool);

      dir->name_buf = svn_stringbuf_create("", dir->dir_baton_pool);
      dir->name = dir->name_buf->data;

      SVN_ERR(dir->update_editor->open_root(dir->update_baton, dir->base_rev,
                                            dir->pool, &dir->dir_baton));
    }
  else
    {
      SVN_ERR(open_dir(dir->parent_dir));

      apr_pool_create(&dir->dir_baton_pool, dir->parent_dir->dir_baton_pool);

      /* Expand our name. */
      dir->name_buf = svn_stringbuf_dup(dir->parent_dir->name_buf,
                                        dir->dir_baton_pool);
      svn_path_add_component(dir->name_buf, dir->base_name);
      dir->name = dir->name_buf->data;

      if (SVN_IS_VALID_REVNUM(dir->base_rev))
        {
          SVN_ERR(dir->update_editor->open_directory(dir->name,
                                         dir->parent_dir->dir_baton,
                                         dir->base_rev, dir->dir_baton_pool,
                                         &dir->dir_baton));
        }
      else
        {
          dir->update_editor->add_directory(dir->name,
                                            dir->parent_dir->dir_baton,
                                            NULL, SVN_INVALID_REVNUM,
                                            dir->dir_baton_pool,
                                            &dir->dir_baton);
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
close_dir(report_dir_t *dir)
{
  report_dir_t *prev, *sibling;

  if (dir->ref_count)
    {
      abort();
    }

  walk_all_props(dir->props, dir->base_name, SVN_INVALID_REVNUM,
                 set_dir_props, dir, dir->dir_baton_pool);
  walk_all_props(dir->props, dir->url, SVN_INVALID_REVNUM, set_dir_props, dir,
                 dir->dir_baton_pool);
  SVN_ERR(dir->update_editor->close_directory(dir->dir_baton,
                                              dir->dir_baton_pool));

  /* remove us from our parent's children list */
  if (dir->parent_dir)
    {
      prev = NULL;
      sibling = dir->parent_dir->children;

      while (sibling != dir)
        {
          prev = sibling;
          sibling = sibling->sibling;
          if (!sibling)
            abort();
        }

      if (!prev)
        {
          dir->parent_dir->children = dir->sibling;
        }
      else
        {
          prev->sibling = dir->sibling;
        }
    }

  apr_pool_destroy(dir->dir_baton_pool);
  apr_pool_destroy(dir->pool);

  return SVN_NO_ERROR;
}

static svn_error_t *close_all_dirs(report_dir_t *dir)
{
  while (dir->children)
    {
      SVN_ERR(close_all_dirs(dir->children));
      dir->ref_count--;
    }

  if (dir->ref_count)
    {
      abort();
    }

  if (!dir->dir_baton)
    {
      SVN_ERR(open_dir(dir));
    }

  return close_dir(dir);
}

static apr_status_t
headers_fetch(serf_bucket_t *headers,
              void *baton,
              apr_pool_t *pool)
{
  report_fetch_t *fetch_ctx = baton;

  /* note that we have old VC URL */
  if (SVN_IS_VALID_REVNUM(fetch_ctx->info->base_rev))
    {
      serf_bucket_headers_setn(headers, SVN_DAV_DELTA_BASE_HEADER,
                               fetch_ctx->info->delta_base->data);
      serf_bucket_headers_setn(headers, "Accept-Encoding",
                               "svndiff1;q=0.9,svndiff;q=0.8");
    }
  else
    {
      serf_bucket_headers_setn(headers, "Accept-Encoding", "gzip");
    }

  return APR_SUCCESS;
}

static apr_status_t
error_fetch(serf_request_t *request,
            serf_bucket_t *response,
            int status_code,
            void *baton)
{
  report_fetch_t *fetch_ctx = baton;

  /* Uh-oh.  Our connection died on us.
   *
   * The core ra_serf layer will requeue our request - we just need to note
   * that we got cut off in the middle of our song.
   */
  if (!response)
    {
      /* If we already started the fetch and opened the file handle, we need
       * to hold subsequent read() ops until we get back to where we were
       * before the close and we can then resume the textdelta() calls.
       */
      if (fetch_ctx->read_headers == TRUE)
        {
          if (fetch_ctx->aborted_read == FALSE && fetch_ctx->read_size)
            {
              fetch_ctx->aborted_read = TRUE;
              fetch_ctx->aborted_read_size = fetch_ctx->read_size;
            }
          fetch_ctx->read_size = 0;
        }

      return APR_SUCCESS;
    }

  /* We have no idea what went wrong. */
  abort();
}

static apr_status_t
handle_fetch(serf_request_t *request,
             serf_bucket_t *response,
             void *handler_baton,
             apr_pool_t *pool)
{
  const char *data;
  apr_size_t len;
  serf_status_line sl;
  apr_status_t status;
  report_fetch_t *fetch_ctx = handler_baton;

  if (fetch_ctx->read_headers == FALSE)
    {
      serf_bucket_t *hdrs;
      const char *val;
      report_info_t *info;
      svn_error_t *err;

      hdrs = serf_bucket_response_get_headers(response);
      val = serf_bucket_headers_get(hdrs, "Content-Type");
      info = fetch_ctx->info;

      if (!info->dir->dir_baton)
        {
          open_dir(info->dir);
        }

      apr_pool_create(&info->editor_pool, info->dir->dir_baton_pool);

      /* Expand our full name now if we haven't done so yet. */
      if (!info->name)
        {
          info->name_buf = svn_stringbuf_dup(info->dir->name_buf,
                                             info->dir->dir_baton_pool);
          svn_path_add_component(info->name_buf, info->base_name);
          info->name = info->name_buf->data;
        }

      if (SVN_IS_VALID_REVNUM(info->base_rev))
        {
          err = info->dir->update_editor->open_file(info->name,
                                          info->dir->dir_baton,
                                          info->base_rev,
                                          info->editor_pool,
                                          &info->file_baton);
        }
      else
        {
          err = info->dir->update_editor->add_file(info->name,
                                          info->dir->dir_baton,
                                          NULL,
                                          info->base_rev,
                                          info->editor_pool,
                                          &info->file_baton);
        }

      info->dir->update_editor->apply_textdelta(info->file_baton,
                                                NULL,
                                                info->editor_pool,
                                                &info->textdelta,
                                                &info->textdelta_baton);

      if (val && strcasecmp(val, "application/vnd.svn-svndiff") == 0)
        {
          fetch_ctx->delta_stream =
              svn_txdelta_parse_svndiff(info->textdelta,
                                        info->textdelta_baton,
                                        TRUE, info->editor_pool);
        }
      else
        {
          fetch_ctx->delta_stream = NULL;
        }

      fetch_ctx->read_headers = TRUE;
    }

  while (1)
    {
      svn_txdelta_window_t delta_window = { 0 };
      svn_txdelta_op_t delta_op;
      svn_string_t window_data;

      status = serf_bucket_read(response, 8000, &data, &len);
      if (SERF_BUCKET_READ_ERROR(status))
        {
          return status;
        }

      fetch_ctx->read_size += len;

      if (fetch_ctx->aborted_read == TRUE)
        {
          /* We haven't caught up to where we were before. */
          if (fetch_ctx->read_size < fetch_ctx->aborted_read_size)
            {
              /* Eek.  What did the file shrink or something? */
              if (APR_STATUS_IS_EOF(status))
                {
                  abort();
                }

              /* Skip on to the next iteration of this loop. */
              if (APR_STATUS_IS_EAGAIN(status))
                {
                  return status;
                }
              continue;
            }

          /* Woo-hoo.  We're back. */ 
          fetch_ctx->aborted_read = FALSE;

          /* Increment data and len by the difference. */
          data += fetch_ctx->read_size - fetch_ctx->aborted_read_size;
          len += fetch_ctx->read_size - fetch_ctx->aborted_read_size;
        }
      
      if (fetch_ctx->delta_stream)
        {
          svn_stream_write(fetch_ctx->delta_stream, data, &len);
        }
      /* otherwise, manually construct the text delta window. */
      else if (len)
        {
          window_data.data = data;
          window_data.len = len;

          delta_op.action_code = svn_txdelta_new;
          delta_op.offset = 0;
          delta_op.length = len;

          delta_window.tview_len = len;
          delta_window.num_ops = 1;
          delta_window.ops = &delta_op;
          delta_window.new_data = &window_data;

          /* write to the file located in the info. */
          fetch_ctx->info->textdelta(&delta_window,
                                     fetch_ctx->info->textdelta_baton);
        }

      if (APR_STATUS_IS_EOF(status))
        {
          report_info_t *info = fetch_ctx->info;

          info->textdelta(NULL, info->textdelta_baton);

          /* set all of the properties we received */
          walk_all_props(info->dir->props,
                         info->base_name,
                         SVN_INVALID_REVNUM,
                         set_file_props,
                         info, info->editor_pool);
          walk_all_props(info->dir->props,
                         info->url,
                         SVN_INVALID_REVNUM,
                         set_file_props,
                         info, info->editor_pool);

          info->dir->update_editor->close_file(info->file_baton, NULL,
                                               info->editor_pool);

          fetch_ctx->done_item.data = fetch_ctx;
          fetch_ctx->done_item.next = *fetch_ctx->done_list;
          *fetch_ctx->done_list = &fetch_ctx->done_item;

          /* We're done with our pools. */
          apr_pool_destroy(info->editor_pool);
          apr_pool_destroy(info->pool);

          return status;
        }
      if (APR_STATUS_IS_EAGAIN(status))
        {
          return status;
        }
    }
  /* not reached */

  abort();
}

static void fetch_file(report_context_t *ctx, report_info_t *info)
{
  const char *checked_in_url, *checksum;
  ra_serf_connection_t *conn;
  ra_serf_handler_t *handler;
  report_fetch_t *fetch_ctx;
  propfind_context_t *prop_ctx = NULL;
  apr_hash_t *props;

  /* What connection should we go on? */
  conn = ctx->sess->conns[ctx->sess->cur_conn];

  /* go fetch info->name from DAV:checked-in */
  checked_in_url = get_prop(info->dir->props, info->base_name,
                         "DAV:", "checked-in");

  if (!checked_in_url)
    {
      abort();
    }

  info->url = checked_in_url;

  /* First, create the PROPFIND to retrieve the properties. */
  deliver_props(&prop_ctx, info->dir->props, ctx->sess, conn,
                info->url, SVN_INVALID_REVNUM, "0", all_props,
                FALSE, &ctx->done_propfinds, info->dir->pool);

  if (!prop_ctx)
    {
      abort();
    }

  ctx->active_propfinds++;

  /* Create the fetch context. */
  fetch_ctx = apr_pcalloc(info->dir->pool, sizeof(*fetch_ctx));
  fetch_ctx->pool = info->pool;
  fetch_ctx->info = info;
  fetch_ctx->done_list = &ctx->done_fetches;
  fetch_ctx->sess = ctx->sess;
  fetch_ctx->conn = conn;

  handler = apr_pcalloc(info->dir->pool, sizeof(*handler));

  handler->method = "GET";
  handler->path = fetch_ctx->info->url;
  handler->conn = conn;
  handler->session = ctx->sess;

  handler->header_delegate = headers_fetch;
  handler->header_delegate_baton = fetch_ctx;

  handler->response_handler = handle_fetch;
  handler->response_baton = fetch_ctx;

  ra_serf_request_create(handler);

  ctx->active_fetches++;
}

static void XMLCALL
start_report(void *userData, const char *name, const char **attrs)
{
  report_context_t *ctx = userData;
  dav_props_t prop_name;
  apr_pool_t *pool;
  ns_t **ns_list;

  if (!ctx->state)
    {
      pool = ctx->sess->pool;
      ns_list = &ctx->ns_list;
    }
  else
    {
      pool = ctx->state->pool;
      ns_list = &ctx->state->ns_list;
    }

  /* check for new namespaces */
  define_ns(ns_list, attrs, pool);

  /* look up name space if present */
  prop_name = expand_ns(*ns_list, name);

  if (!ctx->state && strcmp(prop_name.name, "target-revision") == 0)
    {
      const char *rev;

      rev = find_attr(attrs, "rev");

      if (!rev)
        {
          abort();
        }

      ctx->update_editor->set_target_revision(ctx->update_baton,
                                              SVN_STR_TO_REV(rev),
                                              ctx->sess->pool);
    }
  else if (!ctx->state && strcmp(prop_name.name, "open-directory") == 0)
    {
      const char *rev;
      report_info_t *info;

      rev = find_attr(attrs, "rev");

      if (!rev)
        {
          abort();
        }

      push_state(ctx, OPEN_DIR);

      info = ctx->state->info;

      info->base_rev = apr_atoi64(rev);
      info->dir->base_rev = info->base_rev;

      info->dir->base_name = "";
      info->dir->name = NULL;

      info->base_name = info->dir->base_name;
      info->name = info->dir->name;
    }
  else if (!ctx->state)
    {
      /* do nothing as we haven't seen our valid start tag yet. */
    }
  else if ((ctx->state->state == OPEN_DIR || ctx->state->state == ADD_DIR) &&
           strcmp(prop_name.name, "open-directory") == 0)
    {
      const char *rev, *dirname;
      report_dir_t *dir_info;
      report_info_t *info;

      rev = find_attr(attrs, "rev");

      if (!rev)
        {
          abort();
        }

      dirname = find_attr(attrs, "name");

      if (!dirname)
        {
          abort();
        }

      push_state(ctx, OPEN_DIR);

      info = ctx->state->info;
      dir_info = info->dir;

      info->base_rev = apr_atoi64(rev);
      info->dir->base_rev = info->base_rev;

      info->dir->base_name = apr_pstrdup(info->dir->pool, dirname);
      info->dir->name = NULL;

      ctx->state->info->base_name = dir_info->base_name;
      ctx->state->info->name = dir_info->name;
    }
  else if ((ctx->state->state == OPEN_DIR || ctx->state->state == ADD_DIR) &&
           strcmp(prop_name.name, "add-directory") == 0)
    {
      const char *dir_name;
      report_dir_t *dir_info;

      dir_name = find_attr(attrs, "name");

      push_state(ctx, ADD_DIR);

      dir_info = ctx->state->info->dir;

      dir_info->base_name = apr_pstrdup(dir_info->pool, dir_name);
      dir_info->name = NULL;

      ctx->state->info->base_name = dir_info->base_name;
      ctx->state->info->name = dir_info->name;

      /* Mark that we don't have a base. */
      ctx->state->info->base_rev = SVN_INVALID_REVNUM;
      dir_info->base_rev = ctx->state->info->base_rev;
    }
  else if ((ctx->state->state == OPEN_DIR || ctx->state->state == ADD_DIR) &&
           strcmp(prop_name.name, "open-file") == 0)
    {
      const char *file_name, *rev;
      report_info_t *info;

      file_name = find_attr(attrs, "name");

      if (!file_name)
        {
          abort();
        }

      rev = find_attr(attrs, "rev");

      if (!rev)
        {
          abort();
        }

      push_state(ctx, OPEN_FILE);

      info = ctx->state->info;

      info->base_rev = apr_atoi64(rev);

      info->base_name = apr_pstrdup(info->pool, file_name);
      info->name = NULL;
    }
  else if ((ctx->state->state == OPEN_DIR || ctx->state->state == ADD_DIR) &&
           strcmp(prop_name.name, "add-file") == 0)
    {
      const char *file_name;
      report_info_t *info;

      file_name = find_attr(attrs, "name");

      if (!file_name)
        {
          abort();
        }

      push_state(ctx, ADD_FILE);

      info = ctx->state->info;

      info->base_rev = SVN_INVALID_REVNUM;

      info->base_name = apr_pstrdup(info->pool, file_name);
      info->name = NULL;
    }
  else if ((ctx->state->state == OPEN_DIR || ctx->state->state == ADD_DIR) &&
           strcmp(prop_name.name, "delete-entry") == 0)
    {
      const char *file_name;

      file_name = find_attr(attrs, "name");

      if (!file_name)
        {
          abort();
        }

      if (!ctx->state->info->dir->dir_baton)
        {
          open_dir(ctx->state->info->dir);
        }
      ctx->update_editor->delete_entry(file_name,
                                       SVN_INVALID_REVNUM,
                                       ctx->state->info->dir->dir_baton,
                                       ctx->state->info->dir->pool);
    }
  else if ((ctx->state->state == OPEN_DIR || ctx->state->state == ADD_DIR))
    {
      if (strcmp(prop_name.name, "checked-in") == 0)
        {
          ctx->state->info->prop_ns = prop_name.namespace;
          ctx->state->info->prop_name = apr_pstrdup(ctx->state->pool,
                                                    prop_name.name);
          ctx->state->info->prop_val = NULL;
          push_state(ctx, IGNORE_PROP_NAME);
        }
      else if (strcmp(prop_name.name, "set-prop") == 0)
        {
          const char *full_prop_name;
          dav_props_t new_prop_name;

          full_prop_name = find_attr(attrs, "name");
          new_prop_name = expand_ns(ctx->state->ns_list, full_prop_name);

          ctx->state->info->prop_ns = new_prop_name.namespace;
          ctx->state->info->prop_name = apr_pstrdup(ctx->state->pool,
                                                    new_prop_name.name);
          ctx->state->info->prop_val = NULL;
          push_state(ctx, PROP);
        }
      else if (strcmp(prop_name.name, "prop") == 0)
        {
          /* need to fetch it. */
          push_state(ctx, NEED_PROP_NAME);
        }
      else if (strcmp(prop_name.name, "fetch-props") == 0)
        {
          /* do nothing */
        }
      else
        {
          abort();
        }

    }
  else if ((ctx->state->state == OPEN_FILE || ctx->state->state == ADD_FILE))
    {
      if (strcmp(prop_name.name, "checked-in") == 0)
        {
          ctx->state->info->prop_ns = prop_name.namespace;
          ctx->state->info->prop_name = apr_pstrdup(ctx->state->pool,
                                                    prop_name.name);
          ctx->state->info->prop_val = NULL;
          push_state(ctx, IGNORE_PROP_NAME);
        }
      else if (strcmp(prop_name.name, "prop") == 0)
        {
          /* need to fetch it. */
          push_state(ctx, NEED_PROP_NAME);
        }
    }
  else if (ctx->state->state == IGNORE_PROP_NAME)
    {
      push_state(ctx, PROP);
    }
  else if (ctx->state->state == NEED_PROP_NAME)
    {
      ctx->state->info->prop_ns = prop_name.namespace;
      ctx->state->info->prop_name = apr_pstrdup(ctx->state->pool,
                                                prop_name.name);
      ctx->state->info->prop_val = NULL;
      push_state(ctx, PROP);
    }
}

static void XMLCALL
end_report(void *userData, const char *raw_name)
{
  report_context_t *ctx = userData;
  dav_props_t name;

  if (!ctx->state)
    {
      /* nothing to close yet. */
      return;
    }

  name = expand_ns(ctx->state->ns_list, raw_name);

  if (((ctx->state->state == OPEN_DIR &&
        (strcmp(name.name, "open-directory") == 0)) ||
       (ctx->state->state == ADD_DIR &&
        (strcmp(name.name, "add-directory") == 0))))
    {
      /* At this point, we should have the checked-in href.
       * We need to go do a PROPFIND to get the dir props.
       */
      propfind_context_t *prop_ctx = NULL;
      const char *checked_in_url;
      report_info_t *info = ctx->state->info;

      /* go fetch info->file_name from DAV:checked-in */
      checked_in_url = get_prop(info->dir->props, info->base_name,
                                "DAV:", "checked-in");

      if (!checked_in_url)
        {
          abort();
        }

      info->dir->url = checked_in_url;

      /* First, create the PROPFIND to retrieve the properties. */
      deliver_props(&prop_ctx, info->dir->props, ctx->sess,
                    ctx->sess->conns[ctx->sess->cur_conn],
                    info->dir->url, SVN_INVALID_REVNUM,
                    "0", all_props, FALSE, &ctx->done_propfinds,
                    info->dir->pool);

      if (!prop_ctx)
        {
          abort();
        }

      ctx->active_propfinds++;

      info->dir->propfind = prop_ctx;

      pop_state(ctx);
    }
  else if (ctx->state->state == OPEN_FILE)
    {
      report_info_t *info = ctx->state->info;

      /* At this point, we *must* create our parent's names. */
      if (!info->dir->dir_baton)
        {
          open_dir(info->dir);
        }

      /* Expand our full name now if we haven't done so yet. */
      if (!info->name)
        {
          info->name_buf = svn_stringbuf_dup(info->dir->name_buf,
                                             info->dir->dir_baton_pool);
          svn_path_add_component(info->name_buf, info->base_name);
          info->name = info->name_buf->data;
        }

      /* We now need to dive all the way into the WC to get the base VCC url. */
      ctx->sess->wc_callbacks->get_wc_prop(ctx->sess->wc_callback_baton,
                                           info->name,
                                           RA_SERF_WC_CHECKED_IN_URL,
                                           &info->delta_base,
                                           info->pool);

      fetch_file(ctx, info);
      pop_state(ctx);
    }
  else if (ctx->state->state == ADD_FILE)
    {
      /* We should have everything we need to fetch the file. */
      fetch_file(ctx, ctx->state->info);
      pop_state(ctx);
    }
  else if (ctx->state->state == PROP)
    {
      /* We need to move the prop_ns, prop_name, and prop_val into the
       * same lifetime as the dir->pool.
       */
      ns_t *ns, *ns_name_match;
      int found = 0;
      report_dir_t *dir;

      dir = ctx->state->info->dir;

      /* We're going to be slightly tricky.  We don't care what the ->url
       * field is here at this point.  So, we're going to stick a single
       * copy of the property name inside of the ->url field.
       */
      ns_name_match = NULL;
      for (ns = dir->ns_list; ns; ns = ns->next)
        {
          if (strcmp(ns->namespace, ctx->state->info->prop_ns) == 0)
            {
              ns_name_match = ns;
              if (strcmp(ns->url, ctx->state->info->prop_name) == 0)
                {
                  found = 1;
                  break;
                }
            }
        }

      if (!found)
        {
          ns = apr_palloc(dir->pool, sizeof(*ns));
          if (!ns_name_match)
            {
              ns->namespace = apr_pstrdup(dir->pool, ctx->state->info->prop_ns);
            }
          else
            {
              ns->namespace = ns_name_match->namespace;
            }
          ns->url = apr_pstrdup(dir->pool, ctx->state->info->prop_name);

          ns->next = dir->ns_list;
          dir->ns_list = ns;
        }

      set_prop(dir->props, ctx->state->info->base_name,
               ns->namespace, ns->url,
               apr_pstrmemdup(dir->pool, ctx->state->info->prop_val,
                              ctx->state->info->prop_val_len),
               dir->pool);
      pop_state(ctx);
    }
  else if ((ctx->state->state == IGNORE_PROP_NAME ||
            ctx->state->state == NEED_PROP_NAME))
    {
      pop_state(ctx);
    }
}

static void XMLCALL
cdata_report(void *userData, const char *data, int len)
{
  report_context_t *ctx = userData;
  if (ctx->state && ctx->state->state == PROP)
    {
      expand_string(&ctx->state->info->prop_val,
                    &ctx->state->info->prop_val_len,
                    data, len, ctx->state->pool);

    }
}

static svn_error_t *
set_path(void *report_baton,
         const char *path,
         svn_revnum_t revision,
         svn_boolean_t start_empty,
         const char *lock_token,
         apr_pool_t *pool)
{
  report_context_t *report = report_baton;
  serf_bucket_t *tmp;
  const char *path_copy;

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("<S:entry rev=\"",
                                      sizeof("<S:entry rev=\"")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING(apr_ltoa(report->pool, revision),
                                  report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("\"", sizeof("\"")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  if (lock_token)
    {
      tmp = SERF_BUCKET_SIMPLE_STRING_LEN(" lock-token=\"",
                                          sizeof(" lock-token=\"")-1,
                                          report->sess->bkt_alloc);
      serf_bucket_aggregate_append(report->buckets, tmp);

      tmp = SERF_BUCKET_SIMPLE_STRING(lock_token,
                                      report->sess->bkt_alloc);
      serf_bucket_aggregate_append(report->buckets, tmp);

      tmp = SERF_BUCKET_SIMPLE_STRING_LEN("\"", sizeof("\"")-1,
                                          report->sess->bkt_alloc);
      serf_bucket_aggregate_append(report->buckets, tmp);
    }

  if (start_empty)
    {
      tmp = SERF_BUCKET_SIMPLE_STRING_LEN(" start-empty=\"true\"",
                                          sizeof(" start-empty=\"true\"")-1,
                                          report->sess->bkt_alloc);
      serf_bucket_aggregate_append(report->buckets, tmp);
    }

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN(">", sizeof(">")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  path_copy = apr_pstrdup(report->pool, path);

  tmp = SERF_BUCKET_SIMPLE_STRING(path_copy, report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("</S:entry>",
                                      sizeof("</S:entry>")-1,
                                      report->sess->bkt_alloc);

  serf_bucket_aggregate_append(report->buckets, tmp);
  return APR_SUCCESS;
}

static svn_error_t *
delete_path(void *report_baton,
            const char *path,
            apr_pool_t *pool)
{
  report_context_t *report = report_baton;
  serf_bucket_t *tmp;
  const char *path_copy;

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("<S:missing>",
                                      sizeof("<S:missing>")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  path_copy = apr_pstrdup(report->pool, path);

  tmp = SERF_BUCKET_SIMPLE_STRING(path_copy, report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("</S:missing>",
                                      sizeof("</S:missing>")-1,
                                      report->sess->bkt_alloc);

  serf_bucket_aggregate_append(report->buckets, tmp);
  return APR_SUCCESS;
}

static svn_error_t *
link_path(void *report_baton,
          const char *path,
          const char *url,
          svn_revnum_t revision,
          svn_boolean_t start_empty,
          const char *lock_token,
          apr_pool_t *pool)
{
  report_context_t *report = report_baton;
  abort();
}

static svn_error_t *
finish_report(void *report_baton,
              apr_pool_t *pool)
{
  report_context_t *report = report_baton;
  ra_serf_session_t *sess = report->sess;
  ra_serf_handler_t *handler;
  ra_serf_xml_parser_t *parser_ctx;
  ra_serf_list_t *done_list;
  serf_bucket_t *tmp;
  const char *vcc_url;
  apr_hash_t *props;
  apr_status_t status;
  int i;

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("</S:update-report>",
                                      sizeof("</S:update-report>")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  props = apr_hash_make(pool);

  SVN_ERR(retrieve_props(props, sess, sess->conns[0], sess->repos_url.path,
                         SVN_INVALID_REVNUM, "0",
                         vcc_props, pool));

  vcc_url = get_prop(props, sess->repos_url.path, "DAV:",
                       "version-controlled-configuration");

  if (!vcc_url)
    {
      abort();
    }

  /* create and deliver request */
  report->path = vcc_url;

  handler = apr_pcalloc(pool, sizeof(*handler));

  handler->method = "REPORT";
  handler->path = report->path;
  handler->body_buckets = report->buckets;
  handler->body_type = "text/xml";
  handler->conn = sess->conns[0];
  handler->session = sess;

  parser_ctx = apr_pcalloc(pool, sizeof(*parser_ctx));

  parser_ctx->user_data = report;
  parser_ctx->start = start_report;
  parser_ctx->end = end_report;
  parser_ctx->cdata = cdata_report;
  parser_ctx->done = &report->done;

  handler->response_handler = handle_xml_parser;
  handler->response_baton = parser_ctx;

  ra_serf_request_create(handler);

  for (i = 1; i < 4; i++) {
      sess->conns[i] = apr_palloc(pool, sizeof(*sess->conns[i]));
      sess->conns[i]->bkt_alloc = serf_bucket_allocator_create(sess->pool,
                                                               NULL, NULL);
      sess->conns[i]->address = sess->conns[0]->address;
      sess->conns[i]->hostinfo = sess->conns[0]->hostinfo;
      sess->conns[i]->using_ssl = sess->conns[0]->using_ssl;
      sess->conns[i]->ssl_context = NULL;
      sess->conns[i]->auth_header = sess->auth_header;
      sess->conns[i]->auth_value = sess->auth_value;
      sess->conns[i]->conn = serf_connection_create(sess->context,
                                                    sess->conns[i]->address,
                                                    conn_setup, sess->conns[i],
                                                    conn_closed, sess->conns[i],
                                                    sess->pool);
      sess->num_conns++;
  }

  sess->cur_conn = 1;

  while (!report->done || report->active_fetches || report->active_propfinds)
    {
      status = serf_context_run(sess->context, SERF_DURATION_FOREVER, pool);
      if (APR_STATUS_IS_TIMEUP(status))
        {
          continue;
        }
      if (status)
        {
          return svn_error_wrap_apr(status, _("Error retrieving REPORT"));
        }

      /* Switch our connection. */
      if (!report->done)
         if (++sess->cur_conn == sess->num_conns)
             sess->cur_conn = 1;

      /* prune our propfind list if they are done. */
      done_list = report->done_propfinds;
      while (done_list)
        {
          report->active_propfinds--;

          done_list = done_list->next;
        }
      report->done_propfinds = NULL;

      /* prune our fetches list if they are done. */
      done_list = report->done_fetches;
      while (done_list)
        {
          report_fetch_t *done_fetch = done_list->data;
          report_dir_t *cur_dir;

          /* decrease our parent's directory refcount. */
          cur_dir = done_fetch->info->dir;
          cur_dir->ref_count--;

          /* Decrement our active fetch count. */
          report->active_fetches--;

          done_list = done_list->next;

          /* If our parent has no remaining children and it is not possible
           * for us to add more, it's time for us to close this dir.
           */
          if (!cur_dir->ref_count &&
              cur_dir->propfind && is_propfind_done(cur_dir->propfind))
            {
              do
                {
                  if (cur_dir->parent_dir)
                    {
                      cur_dir->parent_dir->ref_count--;
                    }
                  SVN_ERR(close_dir(cur_dir));
                  cur_dir = cur_dir->parent_dir;
                }
              while (cur_dir && !cur_dir->ref_count && cur_dir->propfind &&
                     is_propfind_done(cur_dir->propfind));
            }
        }
      report->done_fetches = NULL;

      /* Debugging purposes only! */
      serf_debug__closed_conn(sess->bkt_alloc);
    }

  /* This is a funky edge case, but it makes sense:
   * We could have empty directories, so we need to close them.
   */
  if (report->root_dir->ref_count)
    {
      report_dir_t *child;

      /* If we don't have a child dir, something went horribly wrong. */
      if (!report->root_dir->children)
        {
          abort();
        }

      close_all_dirs(report->root_dir);
    }

  /* FIXME subpool */
  SVN_ERR(report->update_editor->close_edit(report->update_baton, sess->pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
abort_report(void *report_baton,
             apr_pool_t *pool)
{
  report_context_t *report = report_baton;
  abort();
}

static const svn_ra_reporter2_t ra_serf_reporter = {
  set_path,
  delete_path,
  link_path,
  finish_report,
  abort_report
};

svn_error_t *
svn_ra_serf__do_update(svn_ra_session_t *ra_session,
                       const svn_ra_reporter2_t **reporter,
                       void **report_baton,
                       svn_revnum_t revision_to_update_to,
                       const char *update_target,
                       svn_boolean_t recurse,
                       const svn_delta_editor_t *update_editor,
                       void *update_baton,
                       apr_pool_t *pool)
{
  report_context_t *report;
  ra_serf_session_t *session = ra_session->priv;
  serf_bucket_t *tmp;

  report = apr_pcalloc(pool, sizeof(*report));
  report->pool = pool;
  report->sess = ra_session->priv;
  report->conn = report->sess->conns[0];
  report->target = update_target;
  report->target_rev = revision_to_update_to;
  report->recurse = recurse;
  report->update_editor = update_editor;
  report->update_baton = update_baton;
  report->done = FALSE;

  *reporter = &ra_serf_reporter;
  *report_baton = report;

  report->buckets = serf_bucket_aggregate_create(report->sess->bkt_alloc);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("<S:update-report xmlns:S=\"",
                                      sizeof("<S:update-report xmlns:S=\"")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN(SVN_XML_NAMESPACE,
                                      sizeof(SVN_XML_NAMESPACE)-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("\">",
                                      sizeof("\">")-1,
                                      report->sess->bkt_alloc);
  serf_bucket_aggregate_append(report->buckets, tmp);

  add_tag_buckets(report->buckets,
                  "S:src-path", report->sess->repos_url.path,
                  report->sess->bkt_alloc);

  if (SVN_IS_VALID_REVNUM(revision_to_update_to))
    {
      add_tag_buckets(report->buckets,
                      "S:target-revision",
                      apr_ltoa(pool, revision_to_update_to),
                      report->sess->bkt_alloc);
    }

  if (*update_target)
    {
      add_tag_buckets(report->buckets,
                      "S:update-target", update_target,
                      report->sess->bkt_alloc);
    }

  if (!recurse)
    {
      add_tag_buckets(report->buckets,
                      "S:recursive", "no",
                      report->sess->bkt_alloc);
    }

  return SVN_NO_ERROR;
}
