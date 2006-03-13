/*
 * commit.c :  entry point for commit RA functions for ra_serf
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
#include "svn_base64.h"
#include "svn_version.h"
#include "svn_path.h"
#include "svn_private_config.h"

#include "ra_serf.h"


/* Structure associated with a MKACTIVITY request. */
typedef struct {

  int status;

  svn_boolean_t done;

} mkactivity_context_t;

/* Structure associated with a CHECKOUT request. */
typedef struct {

  apr_pool_t *pool;

  svn_ra_serf__session_t *session;
  svn_ra_serf__connection_t *conn;

  const char *activity_url;
  apr_size_t activity_url_len;

  const char *checkout_url;

  const char *resource_url;

  int status;

  svn_boolean_t done;

  serf_response_acceptor_t acceptor;
  void *acceptor_baton;
  serf_response_handler_t handler;
} checkout_context_t;

/* Structure associated with a PROPPATCH request. */
typedef struct {
  apr_pool_t *pool;

  svn_ra_serf__session_t *session;
  svn_ra_serf__connection_t *conn;

  const char *path;

  /* Changed and removed properties. */
  apr_hash_t *changed_props;
  apr_hash_t *removed_props;

  int status;

  svn_boolean_t done;

  serf_response_acceptor_t acceptor;
  void *acceptor_baton;
  serf_response_handler_t handler;
} proppatch_context_t;

/* Structure associated with a DELETE/HEAD/etc request. */
typedef struct {
  apr_pool_t *pool;

  svn_ra_serf__session_t *session;
  svn_ra_serf__connection_t *conn;

  const char *path;

  int status;

  svn_boolean_t done;

  serf_response_acceptor_t acceptor;
  void *acceptor_baton;
  serf_response_handler_t handler;
} simple_request_context_t;

/* Baton passed back with the commit editor. */
typedef struct {
  /* Pool for our commit. */
  apr_pool_t *pool;

  svn_ra_serf__session_t *session;
  svn_ra_serf__connection_t *conn;

  svn_string_t *log_msg;

  svn_commit_callback2_t callback;
  void *callback_baton;

  apr_hash_t *lock_tokens;
  svn_boolean_t keep_locks;

  const char *uuid;
  const char *activity_url;
  apr_size_t activity_url_len;

  /* The checkout for the baseline. */
  checkout_context_t *baseline;

  /* The checked-in root to base CHECKOUTs from */
  const char *checked_in_url;
 
  /* The root baseline collection */ 
  const char *baseline_url;
} commit_context_t;

/* Represents a directory. */
typedef struct dir_context_t {
  /* Pool for our directory. */
  apr_pool_t *pool;

  /* The root commit we're in progress for. */
  commit_context_t *commit;

  /* The checked out context for this directory.
   *
   * May be NULL; if so call checkout_dir() first.
   */
  checkout_context_t *checkout;

  /* Our URL to CHECKOUT */
  const char *checked_in_url;

  /* How many pending changes we have left in this directory. */
  unsigned int ref_count;

  /* Our parent */
  struct dir_context_t *parent_dir;

  /* The directory name; if NULL, we're the 'root' */
  const char *name;

  /* The base revision of the dir. */
  svn_revnum_t base_revision;

  const char *copyfrom_path;
  svn_revnum_t copyfrom_revision;

  /* Changed and removed properties */
  apr_hash_t *changed_props;
  apr_hash_t *removed_props;

} dir_context_t;

/* Represents a file to be committed. */
typedef struct {
  /* Pool for our file. */
  apr_pool_t *pool;

  /* The root commit we're in progress for. */
  commit_context_t *commit;

  dir_context_t *parent_dir;

  const char *name;

  /* The checked out context for this file. */
  checkout_context_t *checkout;

  /* The base revision of the file. */
  svn_revnum_t base_revision;

  /* stream */
  svn_stream_t *stream;

  /* Temporary file containing the svndiff. */
  apr_file_t *svndiff;

  /* Our base checksum as reported by the WC. */
  const char *base_checksum;

  /* Our resulting checksum as reported by the WC. */
  const char *result_checksum;

  /* Connection to do the PUT with */
  svn_ra_serf__connection_t *conn;

  /* Changed and removed properties. */
  apr_hash_t *changed_props;
  apr_hash_t *removed_props;

  /* URL to PUT the file at. */
  const char *put_url;

  /* Is our PUT completed? */
  svn_boolean_t put_done;

  /* What was the status code of our PUT? */
  int put_status;

  /* For the PUT... */
  serf_response_acceptor_t acceptor;
  void *acceptor_baton;
  serf_response_handler_t handler;
} file_context_t;


/* Setup routines and handlers for various requests we'll invoke. */

static apr_status_t
handle_status_only(serf_request_t *request,
                   serf_bucket_t *response,
                   int *status_code,
                   svn_boolean_t *done,
                   apr_pool_t *pool)
{
  apr_status_t status;

  status = svn_ra_serf__handler_discard_body(request, response, NULL, pool);

  if (APR_STATUS_IS_EOF(status))
    {
      serf_status_line sl;
      apr_status_t rv;

      rv = serf_bucket_response_status(response, &sl);

      *status_code = sl.code;
      *done = TRUE;
    }

  return status;
}

static apr_status_t
handle_mkactivity(serf_request_t *request,
                  serf_bucket_t *response,
                  void *handler_baton,
                  apr_pool_t *pool)
{
  mkactivity_context_t *ctx = handler_baton;

  return handle_status_only(request, response, &ctx->status, &ctx->done, pool);
}

#define CHECKOUT_HEADER "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:checkout xmlns:D=\"DAV:\"><D:activity-set><D:href>"
  
#define CHECKOUT_TRAILER "</D:href></D:activity-set></D:checkout>"

static apr_status_t
setup_checkout(serf_request_t *request,
               void *setup_baton,
               serf_bucket_t **req_bkt,
               serf_response_acceptor_t *acceptor,
               void **acceptor_baton,
               serf_response_handler_t *handler,
               void **handler_baton,
               apr_pool_t *pool)
{
  checkout_context_t *ctx = setup_baton;
  serf_bucket_t *body_bkt, *tmp_bkt;
  serf_bucket_alloc_t *alloc;

  alloc = serf_request_get_alloc(request);

  body_bkt = serf_bucket_aggregate_create(alloc);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(CHECKOUT_HEADER,
                                          sizeof(CHECKOUT_HEADER) - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(ctx->activity_url,
                                          ctx->activity_url_len,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(CHECKOUT_TRAILER,
                                          sizeof(CHECKOUT_TRAILER) - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  svn_ra_serf__setup_serf_req(request, req_bkt, NULL, ctx->conn,
                 "CHECKOUT", ctx->checkout_url, body_bkt, "text/xml");

  *acceptor = ctx->acceptor;
  *acceptor_baton = ctx->acceptor_baton;
  *handler = ctx->handler;
  *handler_baton = ctx;

  return APR_SUCCESS;
}

static apr_status_t
handle_checkout(serf_request_t *request,
                serf_bucket_t *response,
                void *handler_baton,
                apr_pool_t *pool)
{
  checkout_context_t *ctx = handler_baton;
  apr_status_t status;

  status = handle_status_only(request, response, &ctx->status, &ctx->done,
                              pool);

  /* Get the resulting location. */
  if (ctx->done)
    {
      serf_bucket_t *hdrs;
      apr_uri_t uri;
      const char *location;

      hdrs = serf_bucket_response_get_headers(response);
      location = serf_bucket_headers_get(hdrs, "Location");
      if (!location)
        {
          abort();
        }
      apr_uri_parse(pool, location, &uri);

      ctx->resource_url = apr_pstrdup(ctx->pool, uri.path);
    }

  return status;
}

static svn_error_t *
checkout_dir(dir_context_t *dir)
{
  checkout_context_t *checkout_ctx;

  if (dir->checkout)
    {
      return SVN_NO_ERROR;
    }

  if (dir->parent_dir)
    {
      SVN_ERR(checkout_dir(dir->parent_dir));
    }

  /* Checkout our directory into the activity URL now. */
  checkout_ctx = apr_pcalloc(dir->pool, sizeof(*checkout_ctx));

  checkout_ctx->pool = dir->pool;
  checkout_ctx->session = dir->commit->session;
  checkout_ctx->conn = dir->commit->conn;

  checkout_ctx->acceptor = svn_ra_serf__accept_response;
  checkout_ctx->acceptor_baton = dir->commit->session;
  checkout_ctx->handler = handle_checkout;
  checkout_ctx->activity_url = dir->commit->activity_url;
  checkout_ctx->activity_url_len = dir->commit->activity_url_len;

  /* We could be called twice for the root: once to checkout the baseline;
   * once to checkout the directory itself if we need to do so.
   */
  if (!dir->parent_dir && !dir->commit->baseline)
    {
      checkout_ctx->checkout_url = dir->commit->baseline_url;
      dir->commit->baseline = checkout_ctx;
    }
  else
    {
      checkout_ctx->checkout_url = dir->checked_in_url;
      dir->checkout = checkout_ctx;
    }

  serf_connection_request_create(checkout_ctx->conn->conn,
                                 setup_checkout, checkout_ctx);

  SVN_ERR(svn_ra_serf__context_run_wait(&checkout_ctx->done,
                                        dir->commit->session,
                                        dir->pool));

  if (checkout_ctx->status != 201)
    {
      abort();
    }

  return SVN_NO_ERROR;
}

static void proppatch_walker(void *baton,
                             const void *ns, apr_ssize_t ns_len,
                             const void *name, apr_ssize_t name_len,
                             const void *val,
                             apr_pool_t *pool)
{
  serf_bucket_t *body_bkt = baton;
  serf_bucket_t *tmp_bkt;
  serf_bucket_alloc_t *alloc;
  const svn_string_t *prop_value;
  svn_boolean_t binary_prop;
 
  prop_value = val;
  if (svn_xml_is_xml_safe(prop_value->data, prop_value->len))
    {
      binary_prop = FALSE;
    }
  else
    {
      binary_prop = TRUE;
    }

  alloc = body_bkt->allocator;

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("<",
                                          sizeof("<") - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(name, name_len, alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(" xmlns=\"",
                                          sizeof(" xmlns=\"") - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(ns, ns_len, alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  if (binary_prop == TRUE)
    {
      tmp_bkt =
          SERF_BUCKET_SIMPLE_STRING_LEN(" V:encoding:=\"base64\"",
                                        sizeof(" V:encoding:=\"base64\"") - 1,
                                        alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);
    }

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("\">",
                                          sizeof("\">") - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  if (binary_prop == TRUE)
    {
      prop_value = svn_base64_encode_string(prop_value, pool);
    }
  else
    {
      svn_stringbuf_t *prop_buf = svn_stringbuf_create("", pool);
      svn_xml_escape_cdata_string(&prop_buf, prop_value, pool);
      prop_value = svn_string_create_from_buf(prop_buf, pool);
    }

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(prop_value->data, prop_value->len,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("</",
                                          sizeof("</") - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(name, name_len, alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(">",
                                          sizeof(">") - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);
}

#define PROPPATCH_HEADER "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:propertyupdate xmlns:D=\"DAV:\" xmlns:V=\"" SVN_DAV_PROP_NS_DAV "\">"

#define PROPPATCH_TRAILER "</D:propertyupdate>"

static apr_status_t
setup_proppatch(serf_request_t *request,
                void *setup_baton,
                serf_bucket_t **req_bkt,
                serf_response_acceptor_t *acceptor,
                void **acceptor_baton,
                serf_response_handler_t *handler,
                void **handler_baton,
                apr_pool_t *pool)
{
  proppatch_context_t *ctx = setup_baton;
  serf_bucket_t *body_bkt, *tmp_bkt;
  serf_bucket_alloc_t *alloc;

  alloc = serf_request_get_alloc(request);

  body_bkt = serf_bucket_aggregate_create(alloc);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(PROPPATCH_HEADER,
                                          sizeof(PROPPATCH_HEADER) - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  if (apr_hash_count(ctx->changed_props) > 0)
    {
      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("<D:set>",
                                              sizeof("<D:set>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);

      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("<D:prop>",
                                              sizeof("<D:prop>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);

      svn_ra_serf__walk_all_props(ctx->changed_props, ctx->path,
                                  SVN_INVALID_REVNUM,
                                  proppatch_walker, body_bkt, pool);

      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("</D:prop>",
                                              sizeof("</D:prop>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);

      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("</D:set>",
                                              sizeof("</D:set>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);
    }

  if (apr_hash_count(ctx->removed_props) > 0)
    {
      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("<D:remove>",
                                              sizeof("<D:remove>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);

      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("<D:prop>",
                                              sizeof("<D:prop>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);

      svn_ra_serf__walk_all_props(ctx->removed_props, ctx->path,
                                  SVN_INVALID_REVNUM,
                                  proppatch_walker, body_bkt, pool);

      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("</D:prop>",
                                              sizeof("</D:prop>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);

      tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN("</D:remove>",
                                              sizeof("</D:remove>") - 1,
                                              alloc);
      serf_bucket_aggregate_append(body_bkt, tmp_bkt);
    }

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(PROPPATCH_TRAILER,
                                          sizeof(PROPPATCH_TRAILER) - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  svn_ra_serf__setup_serf_req(request, req_bkt, NULL, ctx->conn,
                 "PROPPATCH", ctx->path, body_bkt, "text/xml");

  *handler = ctx->handler;
  *handler_baton = ctx;

  return APR_SUCCESS;
}

static apr_status_t
handle_proppatch(serf_request_t *request,
                 serf_bucket_t *response,
                 void *handler_baton,
                 apr_pool_t *pool)
{
  proppatch_context_t *ctx = handler_baton;
  apr_status_t status;

  return handle_status_only(request, response, &ctx->status, &ctx->done, pool);
}

static apr_status_t
setup_put(serf_request_t *request,
          void *setup_baton,
          serf_bucket_t **req_bkt,
          serf_response_acceptor_t *acceptor,
          void **acceptor_baton,
          serf_response_handler_t *handler,
          void **handler_baton,
          apr_pool_t *pool)
{
  file_context_t *ctx = setup_baton;
  serf_bucket_alloc_t *alloc;
  serf_bucket_t *body_bkt, *hdrs_bkt;
  apr_off_t offset;

  alloc = serf_request_get_alloc(request);

  /* We need to flush the file, make it unbuffered (so that it can be
   * zero-copied via mmap), and reset the position before attempting to
   * deliver the file.
   */
  apr_file_flush(ctx->svndiff);
  apr_file_buffer_set(ctx->svndiff, NULL, 0);
  offset = 0;
  apr_file_seek(ctx->svndiff, APR_SET, &offset);

  body_bkt = serf_bucket_file_create(ctx->svndiff, alloc);

  svn_ra_serf__setup_serf_req(request, req_bkt, &hdrs_bkt, ctx->conn,
                 "PUT", ctx->put_url, body_bkt,
                 "application/vnd.svn-svndiff");

  if (ctx->base_checksum)
  {
    serf_bucket_headers_set(hdrs_bkt, SVN_DAV_BASE_FULLTEXT_MD5_HEADER,
                            ctx->base_checksum);
  }

  if (ctx->result_checksum)
  {
    serf_bucket_headers_set(hdrs_bkt, SVN_DAV_RESULT_FULLTEXT_MD5_HEADER,
                            ctx->result_checksum);
  }

  *acceptor = ctx->acceptor;
  *acceptor_baton = ctx->acceptor_baton;
  *handler = ctx->handler;
  *handler_baton = ctx;

  return APR_SUCCESS;
}

static apr_status_t
handle_put(serf_request_t *request,
           serf_bucket_t *response,
           void *handler_baton,
           apr_pool_t *pool)
{
  file_context_t *ctx = handler_baton;

  return handle_status_only(request, response,
                            &ctx->put_status, &ctx->put_done, pool);
}

static apr_status_t
setup_delete(serf_request_t *request,
             void *setup_baton,
             serf_bucket_t **req_bkt,
             serf_response_acceptor_t *acceptor,
             void **acceptor_baton,
             serf_response_handler_t *handler,
             void **handler_baton,
             apr_pool_t *pool)
{
  simple_request_context_t *ctx = setup_baton;

  svn_ra_serf__setup_serf_req(request, req_bkt, NULL, ctx->conn,
                 "DELETE", ctx->path, NULL, NULL);

  *acceptor = ctx->acceptor;
  *acceptor_baton = ctx->acceptor_baton;
  *handler = ctx->handler;
  *handler_baton = ctx;

  return APR_SUCCESS;
}

static apr_status_t
handle_delete(serf_request_t *request,
              serf_bucket_t *response,
              void *handler_baton,
              apr_pool_t *pool)
{
  simple_request_context_t *ctx = handler_baton;

  return handle_status_only(request, response, &ctx->status, &ctx->done, pool);
}

static apr_status_t
setup_head(serf_request_t *request,
           void *setup_baton,
           serf_bucket_t **req_bkt,
           serf_response_acceptor_t *acceptor,
           void **acceptor_baton,
           serf_response_handler_t *handler,
           void **handler_baton,
           apr_pool_t *pool)
{
  simple_request_context_t *ctx = setup_baton;

  svn_ra_serf__setup_serf_req(request, req_bkt, NULL, ctx->conn,
                 "HEAD", ctx->path, NULL, NULL);

  *acceptor = ctx->acceptor;
  *acceptor_baton = ctx->acceptor_baton;
  *handler = ctx->handler;
  *handler_baton = ctx;

  return APR_SUCCESS;
}

serf_bucket_t*
accept_head(serf_request_t *request,
            serf_bucket_t *stream,
            void *acceptor_baton,
            apr_pool_t *pool)
{
  serf_bucket_t *response;
    
  response = svn_ra_serf__accept_response(request, stream, acceptor_baton,
                                          pool);

  /* We know we shouldn't get a response body. */
  serf_bucket_response_set_head(response);

  return response;
}

static apr_status_t
handle_head(serf_request_t *request,
            serf_bucket_t *response,
            void *handler_baton,
            apr_pool_t *pool)
{
  simple_request_context_t *ctx = handler_baton;

  return handle_status_only(request, response, &ctx->status, &ctx->done, pool);
}

/* Helper function to write the svndiff stream to temporary file. */
static svn_error_t *
svndiff_stream_write(void *file_baton,
                     const char *data,
                     apr_size_t *len)
{
  file_context_t *ctx = file_baton;
  apr_status_t status;

  status = apr_file_write_full(ctx->svndiff, data, *len, NULL);
  if (status)
      return svn_error_wrap_apr(status, _("Failed writing updated file"));

  return SVN_NO_ERROR;
}



/* Commit baton callbacks */

static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision,
          apr_pool_t *dir_pool,
          void **root_baton)
{
  commit_context_t *ctx = edit_baton;
  svn_ra_serf__options_context_t *opt_ctx;
  svn_ra_serf__handler_t *handler;
  mkactivity_context_t *mkact_ctx;
  checkout_context_t *checkout_ctx;
  proppatch_context_t *proppatch_ctx;
  dir_context_t *dir;
  const char *activity_str;
  const char *vcc_url, *version_name;
  svn_boolean_t *opt_done;
  apr_hash_t *props;

  /* Create a UUID for this commit. */
  ctx->uuid = svn_uuid_generate(ctx->pool);

  svn_ra_serf__create_options_req(&opt_ctx, ctx->session,
                                  ctx->session->conns[0],
                                  ctx->session->repos_url.path, ctx->pool);

  SVN_ERR(svn_ra_serf__context_run_wait(
                                svn_ra_serf__get_options_done_ptr(opt_ctx),
                                ctx->session, ctx->pool));

  activity_str = svn_ra_serf__options_get_activity_collection(opt_ctx);

  if (!activity_str)
    {
      abort();
    }

  ctx->activity_url = svn_path_url_add_component(activity_str,
                                                 ctx->uuid, ctx->pool);
  ctx->activity_url_len = strlen(ctx->activity_url);

  /* Create our activity URL now on the server. */

  handler = apr_pcalloc(ctx->pool, sizeof(*handler));
  handler->method = "MKACTIVITY";
  handler->path = ctx->activity_url;
  handler->conn = ctx->session->conns[0];
  handler->session = ctx->session;

  mkact_ctx = apr_pcalloc(ctx->pool, sizeof(*mkact_ctx));

  handler->response_handler = handle_mkactivity;
  handler->response_baton = mkact_ctx;

  svn_ra_serf__request_create(handler);

  SVN_ERR(svn_ra_serf__context_run_wait(&mkact_ctx->done, ctx->session,
                                        ctx->pool));

  if (mkact_ctx->status != 201)
    {
      abort();
    }

  /* Now go fetch our VCC and baseline so we can do a CHECKOUT. */
  props = apr_hash_make(ctx->pool);

  SVN_ERR(svn_ra_serf__retrieve_props(props,
                                      ctx->session, ctx->session->conns[0],
                                      ctx->session->repos_url.path,
                                      SVN_INVALID_REVNUM, "0", base_props,
                                      ctx->pool));

  vcc_url = svn_ra_serf__get_prop(props, ctx->session->repos_url.path,
                                  "DAV:", "version-controlled-configuration");

  if (!vcc_url)
    {
      abort();
    }

  SVN_ERR(svn_ra_serf__retrieve_props(props,
                                      ctx->session, ctx->session->conns[0],
                                      ctx->session->repos_url.path,
                                      SVN_INVALID_REVNUM, "0",
                                      checked_in_props, ctx->pool));

  ctx->checked_in_url = svn_ra_serf__get_prop(props,
                                              ctx->session->repos_url.path,
                                              "DAV:", "checked-in");

  if (!ctx->checked_in_url)
    {
      abort();
    }

  /* Using the version-controlled-configuration, fetch the checked-in prop. */
  SVN_ERR(svn_ra_serf__retrieve_props(props,
                                      ctx->session, ctx->session->conns[0],
                                      vcc_url, SVN_INVALID_REVNUM, "0",
                                      checked_in_props, ctx->pool));

  ctx->baseline_url = svn_ra_serf__get_prop(props, vcc_url,
                                            "DAV:", "checked-in");

  if (!ctx->baseline_url)
    {
      abort();
    }

  dir = apr_pcalloc(dir_pool, sizeof(*dir));

  dir->pool = dir_pool;
  dir->commit = ctx;
  dir->base_revision = base_revision;
  dir->name = "";
  dir->checked_in_url = ctx->checked_in_url;
  dir->changed_props = apr_hash_make(dir->pool);
  dir->removed_props = apr_hash_make(dir->pool);

  /* Checkout our root dir */
  checkout_dir(dir);

  /* PROPPATCH our log message and pass it along.  */
  proppatch_ctx = apr_pcalloc(dir_pool, sizeof(*proppatch_ctx));

  proppatch_ctx->pool = dir_pool;
  proppatch_ctx->session = dir->commit->session;
  proppatch_ctx->conn = dir->commit->conn;

  proppatch_ctx->acceptor = svn_ra_serf__accept_response;
  proppatch_ctx->acceptor_baton = dir->commit->session;
  proppatch_ctx->handler = handle_proppatch;

  proppatch_ctx->path = ctx->baseline->resource_url;
  proppatch_ctx->changed_props = apr_hash_make(dir_pool);
  proppatch_ctx->removed_props = apr_hash_make(dir_pool);

  svn_ra_serf__set_prop(proppatch_ctx->changed_props, proppatch_ctx->path,
                        SVN_DAV_PROP_NS_SVN, "log",
                        dir->commit->log_msg, proppatch_ctx->pool);

  serf_connection_request_create(proppatch_ctx->conn->conn,
                                 setup_proppatch, proppatch_ctx);

  *root_baton = dir;

  return SVN_NO_ERROR;
}

static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  dir_context_t *dir = parent_baton;
  simple_request_context_t *delete_ctx;

  /* Ensure our directory has been checked out */
  checkout_dir(dir);

  /* DELETE our activity */
  delete_ctx = apr_pcalloc(pool, sizeof(*delete_ctx));

  delete_ctx->pool = pool;
  delete_ctx->session = dir->commit->session;
  delete_ctx->conn = dir->commit->conn;

  delete_ctx->acceptor = svn_ra_serf__accept_response;
  delete_ctx->acceptor_baton = dir->commit->session;
  delete_ctx->handler = handle_delete;
  delete_ctx->path =
      svn_path_url_add_component(dir->checkout->resource_url,
                                 path, delete_ctx->pool);

  serf_connection_request_create(delete_ctx->conn->conn,
                                 setup_delete, delete_ctx);

  SVN_ERR(svn_ra_serf__context_run_wait(&delete_ctx->done,
                                        dir->commit->session, pool));

  if (delete_ctx->status != 204)
    {
      abort();
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_revision,
              apr_pool_t *dir_pool,
              void **child_baton)
{
  dir_context_t *parent = parent_baton;
  dir_context_t *dir;
  svn_ra_serf__handler_t *handler;
  mkactivity_context_t *mkcol_ctx;

  /* Ensure our parent is checked out. */
  SVN_ERR(checkout_dir(parent));

  dir = apr_pcalloc(dir_pool, sizeof(*dir));

  dir->pool = dir_pool;

  dir->parent_dir = parent;
  dir->commit = parent->commit;

  dir->base_revision = SVN_INVALID_REVNUM;
  dir->copyfrom_revision = copyfrom_revision;
  dir->copyfrom_path = copyfrom_path;
  dir->name = path;
  dir->checked_in_url =
      svn_path_url_add_component(parent->commit->checked_in_url,
                                 path, dir->pool);
  dir->changed_props = apr_hash_make(dir->pool);
  dir->removed_props = apr_hash_make(dir->pool);

  if (copyfrom_path)
    {
      abort();
    }

  handler = apr_pcalloc(dir->pool, sizeof(*handler));
  handler->method = "MKCOL";
  handler->path = svn_path_url_add_component(parent->checkout->resource_url,
                                             svn_path_basename(path, dir->pool),
                                             dir->pool);
  handler->conn = dir->commit->session->conns[0];
  handler->session = dir->commit->session;

  mkcol_ctx = apr_pcalloc(dir->pool, sizeof(*mkcol_ctx));

  handler->response_handler = handle_mkactivity;
  handler->response_baton = mkcol_ctx;

  svn_ra_serf__request_create(handler);

  SVN_ERR(svn_ra_serf__context_run_wait(&mkcol_ctx->done, dir->commit->session,
                                        dir->pool));

  if (mkcol_ctx->status != 201)
    {
      abort();
    }

  *child_baton = dir;

  return SVN_NO_ERROR;
}

static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *dir_pool,
               void **child_baton)
{
  dir_context_t *parent = parent_baton;
  dir_context_t *dir;

  dir = apr_pcalloc(dir_pool, sizeof(*dir));

  dir->pool = dir_pool;

  dir->parent_dir = parent;
  dir->commit = parent->commit;

  dir->base_revision = base_revision;
  dir->name = path;
  dir->checked_in_url =
      svn_path_url_add_component(parent->commit->checked_in_url,
                                 path, dir->pool);
  dir->changed_props = apr_hash_make(dir->pool);
  dir->removed_props = apr_hash_make(dir->pool);

  *child_baton = dir;

  return SVN_NO_ERROR;
}

static svn_error_t *
change_dir_prop(void *dir_baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  dir_context_t *dir = dir_baton;
  const char *ns;

  /* Ensure we have a checked out dir. */
  SVN_ERR(checkout_dir(dir));

  name = apr_pstrdup(dir->pool, name);
  value = svn_string_dup(value, dir->pool);

  if (strncmp(name, SVN_PROP_PREFIX, sizeof(SVN_PROP_PREFIX) - 1) == 0)
    {
      ns = SVN_DAV_PROP_NS_SVN;
      name += sizeof(SVN_PROP_PREFIX) - 1;
    }
  else
    {
      ns = SVN_DAV_PROP_NS_CUSTOM;
    }

  if (value)
    {
      svn_ra_serf__set_prop(dir->changed_props, dir->checkout->resource_url,
                            ns, name, value, dir->pool);
    }
  else
    {
      value = svn_string_create("", dir->pool);

      svn_ra_serf__set_prop(dir->removed_props, dir->checkout->resource_url,
                            ns, name, value, dir->pool);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
close_directory(void *dir_baton,
                apr_pool_t *pool)
{
  dir_context_t *dir = dir_baton;
  proppatch_context_t *proppatch_ctx;

  /* Huh?  We're going to be called before the texts are sent.  Ugh.
   * Therefore, just wave politely at our caller.
   */

  /* PROPPATCH our prop change and pass it along.  */
  if (apr_hash_count(dir->changed_props) ||
      apr_hash_count(dir->removed_props))
    {
      proppatch_ctx = apr_pcalloc(pool, sizeof(*proppatch_ctx));
      proppatch_ctx->pool = pool;
     
      proppatch_ctx->session = dir->commit->session;
      proppatch_ctx->conn = dir->commit->conn;
      
      proppatch_ctx->acceptor = svn_ra_serf__accept_response;
      proppatch_ctx->acceptor_baton = dir->commit->session;
      proppatch_ctx->handler = handle_proppatch;
      
      proppatch_ctx->path = dir->checkout->resource_url;
      proppatch_ctx->changed_props = dir->changed_props;
      proppatch_ctx->removed_props = dir->removed_props;
      
      serf_connection_request_create(proppatch_ctx->conn->conn,
                                     setup_proppatch, proppatch_ctx);
     
      /* If we don't wait for the response, our pool will be gone! */ 
      SVN_ERR(svn_ra_serf__context_run_wait(&proppatch_ctx->done,
                                            dir->commit->session, dir->pool));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
absent_directory(const char *path,
                 void *parent_baton,
                 apr_pool_t *pool)
{
  dir_context_t *ctx = parent_baton;

  abort();
}

static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copy_path,
         svn_revnum_t copy_revision,
         apr_pool_t *file_pool,
         void **file_baton)
{
  dir_context_t *dir = parent_baton;
  simple_request_context_t *head_ctx;
  file_context_t *new_file;

  /* Ensure our directory has been checked out */
  checkout_dir(dir);

  new_file = apr_pcalloc(file_pool, sizeof(*new_file));

  new_file->pool = file_pool;

  dir->ref_count++;
  new_file->parent_dir = dir;

  new_file->commit = dir->commit;
  
  /* TODO: Remove directory names? */
  new_file->name = path;

  new_file->base_revision = SVN_INVALID_REVNUM;
  new_file->changed_props = apr_hash_make(new_file->pool);
  new_file->removed_props = apr_hash_make(new_file->pool);

  /* Set up so that we can perform the PUT later. */
  new_file->acceptor = svn_ra_serf__accept_response;
  new_file->acceptor_baton = dir->commit->session;
  new_file->handler = handle_put;

  /* Ensure that the file doesn't exist by doing a HEAD on the resource. */
  head_ctx = apr_pcalloc(new_file->pool, sizeof(*head_ctx));

  head_ctx->pool = new_file->pool;
  head_ctx->session = new_file->commit->session;
  head_ctx->conn = new_file->commit->conn;

  head_ctx->acceptor = accept_head;
  head_ctx->acceptor_baton = new_file->commit->session;
  head_ctx->handler = handle_head;
  head_ctx->path = 
      svn_path_url_add_component(new_file->commit->session->repos_url.path,
                                 path, new_file->pool);

  serf_connection_request_create(head_ctx->conn->conn,
                                 setup_head, head_ctx);

  SVN_ERR(svn_ra_serf__context_run_wait(&head_ctx->done, dir->commit->session,
                                        new_file->pool));

  if (head_ctx->status != 404)
    { 
      abort();
    }

  if (copy_path)
    {
      /* Issue a COPY */
      abort();
    }

  new_file->conn = new_file->commit->conn;

  new_file->put_url =
      svn_path_url_add_component(dir->checkout->resource_url,
                                 svn_path_basename(path, new_file->pool),
                                 new_file->pool);

  *file_baton = new_file;

  return SVN_NO_ERROR;
}

static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *file_pool,
          void **file_baton)
{
  dir_context_t *ctx = parent_baton;
  file_context_t *new_file;
  checkout_context_t *checkout_ctx;
  apr_hash_t *props;

  new_file = apr_pcalloc(file_pool, sizeof(*new_file));

  new_file->pool = file_pool;

  ctx->ref_count++;
  new_file->parent_dir = ctx;

  new_file->commit = ctx->commit;
  
  /* TODO: Remove directory names? */
  new_file->name = path;

  new_file->base_revision = base_revision;

  /* Set up so that we can perform the PUT later. */
  new_file->conn = new_file->commit->conn;
  new_file->acceptor = svn_ra_serf__accept_response;
  new_file->acceptor_baton = ctx->commit->session;
  new_file->handler = handle_put;
  new_file->changed_props = apr_hash_make(new_file->pool);
  new_file->removed_props = apr_hash_make(new_file->pool);

  /* CHECKOUT the file into our activity. */
  checkout_ctx = apr_pcalloc(new_file->pool, sizeof(*checkout_ctx));

  checkout_ctx->pool = new_file->pool;
  checkout_ctx->session = new_file->commit->session;
  checkout_ctx->conn = new_file->commit->conn;

  checkout_ctx->acceptor = svn_ra_serf__accept_response;
  checkout_ctx->acceptor_baton = new_file->commit->session;
  checkout_ctx->handler = handle_checkout;
  checkout_ctx->activity_url = new_file->commit->activity_url;
  checkout_ctx->activity_url_len = new_file->commit->activity_url_len;

  /* Append our file name to the baseline to get the resulting checkout. */
  checkout_ctx->checkout_url =
      svn_path_url_add_component(new_file->commit->checked_in_url,
                                 path, new_file->pool);

  serf_connection_request_create(checkout_ctx->conn->conn,
                                 setup_checkout, checkout_ctx);

  /* There's no need to wait here as we only need this when we start the
   * PROPPATCH or PUT of the file.
   */
  SVN_ERR(svn_ra_serf__context_run_wait(&checkout_ctx->done,
                                        new_file->commit->session,
                                        new_file->pool));

  if (checkout_ctx->status != 201)
    {
      abort();
    }

  new_file->checkout = checkout_ctx;
  new_file->put_url = checkout_ctx->resource_url;

  *file_baton = new_file;

  return SVN_NO_ERROR;
}

static svn_error_t *
apply_textdelta(void *file_baton,
                const char *base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  file_context_t *ctx = file_baton;
  const svn_ra_callbacks2_t *wc_callbacks;
  void *wc_callback_baton;

  /* Store the stream in a temporary file; we'll give it to serf when we
   * close this file.
   *
   * TODO: There should be a way we can stream the request body instead of
   * writing to a temporary file (ugh). A special svn stream serf bucket
   * that returns EAGAIN until we receive the done call?  But, when
   * would we run through the serf context?  Grr.
   */
  wc_callbacks = ctx->commit->session->wc_callbacks;
  wc_callback_baton = ctx->commit->session->wc_callback_baton;
  SVN_ERR(wc_callbacks->open_tmp_file(&ctx->svndiff,
                                      wc_callback_baton,
                                      ctx->pool));

  ctx->stream = svn_stream_create(ctx, pool);
  svn_stream_set_write(ctx->stream, svndiff_stream_write);

  svn_txdelta_to_svndiff(ctx->stream, pool, handler, handler_baton);

  ctx->base_checksum = base_checksum;

  return SVN_NO_ERROR;
}

static svn_error_t *
change_file_prop(void *file_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  file_context_t *file = file_baton;
  const char *ns;

  name = apr_pstrdup(file->pool, name);
  value = svn_string_dup(value, file->pool);

  if (strncmp(name, SVN_PROP_PREFIX, sizeof(SVN_PROP_PREFIX) - 1) == 0)
    {
      ns = SVN_DAV_PROP_NS_SVN;
      name += sizeof(SVN_PROP_PREFIX) - 1;
    }
  else
    {
      ns = SVN_DAV_PROP_NS_CUSTOM;
    }

  if (value)
    {
      svn_ra_serf__set_prop(file->changed_props, file->put_url,
                            ns, name, value, file->pool);
    }
  else
    {
      value = svn_string_create("", file->pool);

      svn_ra_serf__set_prop(file->removed_props, file->put_url,
                            ns, name, value, file->pool);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
close_file(void *file_baton,
           const char *text_checksum,
           apr_pool_t *pool)
{
  file_context_t *ctx = file_baton;

  ctx->result_checksum = text_checksum;

  /* If we had a stream of changes, push them to the server... */
  if (ctx->stream)
    {
      serf_connection_request_create(ctx->conn->conn,
                                     setup_put, ctx);

      SVN_ERR(svn_ra_serf__context_run_wait(&ctx->put_done,
                                            ctx->commit->session,
                                            ctx->pool));

      if ((ctx->checkout && ctx->put_status != 204) &&
          (!ctx->checkout && ctx->put_status != 201))
        {
          abort();
        }
    }

  /* If we had any prop changes, push them via PROPPATCH. */
  if (apr_hash_count(ctx->changed_props) ||
      apr_hash_count(ctx->removed_props))
    {
      proppatch_context_t *proppatch_ctx;

      proppatch_ctx = apr_pcalloc(ctx->pool, sizeof(*proppatch_ctx));
      proppatch_ctx->pool = ctx->pool;
     
      proppatch_ctx->session = ctx->commit->session;
      proppatch_ctx->conn = ctx->commit->conn;
      
      proppatch_ctx->acceptor = svn_ra_serf__accept_response;
      proppatch_ctx->acceptor_baton = ctx->commit->session;
      proppatch_ctx->handler = handle_proppatch;
      
      proppatch_ctx->path = ctx->put_url;
      proppatch_ctx->changed_props = ctx->changed_props;
      proppatch_ctx->removed_props = ctx->removed_props;
      
      serf_connection_request_create(proppatch_ctx->conn->conn,
                                     setup_proppatch, proppatch_ctx);
     
      /* If we don't wait for the response, our pool will be gone! */ 
      SVN_ERR(svn_ra_serf__context_run_wait(&proppatch_ctx->done,
                                            ctx->commit->session,
                                            ctx->pool));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
absent_file(const char *path,
            void *parent_baton,
            apr_pool_t *pool)
{
  dir_context_t *ctx = parent_baton;

  abort();
}

static svn_error_t *
close_edit(void *edit_baton, 
           apr_pool_t *pool)
{
  commit_context_t *ctx = edit_baton;
  svn_ra_serf__merge_context_t *merge_ctx;
  simple_request_context_t *delete_ctx;
  svn_boolean_t *merge_done;
  apr_status_t status;

  /* MERGE our activity */
  SVN_ERR(svn_ra_serf__merge_create_req(&merge_ctx, ctx->session,
                                        ctx->session->conns[0],
                                        ctx->session->repos_url.path,
                                        ctx->activity_url,
                                        ctx->activity_url_len, pool));

  merge_done = svn_ra_serf__merge_get_done_ptr(merge_ctx);
 
  SVN_ERR(svn_ra_serf__context_run_wait(merge_done, ctx->session, pool));

  if (svn_ra_serf__merge_get_status(merge_ctx) != 200)
    {
      abort();
    }

  /* Inform the WC that we did a commit.  */
  ctx->callback(svn_ra_serf__merge_get_commit_info(merge_ctx),
                ctx->callback_baton, pool);

  /* DELETE our activity */
  delete_ctx = apr_pcalloc(pool, sizeof(*delete_ctx));

  delete_ctx->pool = pool;
  delete_ctx->session = ctx->session;
  delete_ctx->conn = ctx->conn;

  delete_ctx->acceptor = svn_ra_serf__accept_response;
  delete_ctx->acceptor_baton = ctx->session;
  delete_ctx->handler = handle_delete;
  delete_ctx->path = ctx->activity_url;

  serf_connection_request_create(delete_ctx->conn->conn,
                                 setup_delete, delete_ctx);

  SVN_ERR(svn_ra_serf__context_run_wait(&delete_ctx->done, ctx->session, pool));

  if (delete_ctx->status != 204)
    {
      abort();
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
abort_edit(void *edit_baton,
           apr_pool_t *pool)
{
  commit_context_t *ctx = edit_baton;

  abort();
}

svn_error_t *
svn_ra_serf__get_commit_editor(svn_ra_session_t *ra_session,
                               const svn_delta_editor_t **ret_editor,
                               void **edit_baton,
                               const char *log_msg,
                               svn_commit_callback2_t callback,
                               void *callback_baton,
                               apr_hash_t *lock_tokens,
                               svn_boolean_t keep_locks,
                               apr_pool_t *pool)
{
  svn_ra_serf__session_t *session = ra_session->priv;
  svn_delta_editor_t *editor;
  commit_context_t *ctx;

  ctx = apr_pcalloc(pool, sizeof(commit_context_t));

  ctx->pool = pool;

  ctx->session = session;
  ctx->conn = session->conns[0];
  ctx->log_msg = svn_string_create(log_msg, pool);

  ctx->callback = callback;
  ctx->callback_baton = callback_baton;

  ctx->lock_tokens = lock_tokens;
  ctx->keep_locks = keep_locks;

  editor = svn_delta_default_editor(pool);
  editor->open_root = open_root;
  editor->delete_entry = delete_entry;
  editor->add_directory = add_directory;
  editor->open_directory = open_directory;
  editor->change_dir_prop = change_dir_prop;
  editor->close_directory = close_directory;
  editor->absent_directory = absent_directory;
  editor->add_file = add_file;
  editor->open_file = open_file;
  editor->apply_textdelta = apply_textdelta;
  editor->change_file_prop = change_file_prop;
  editor->close_file = close_file;
  editor->absent_file = absent_file;
  editor->close_edit = close_edit;
  editor->abort_edit = abort_edit;

  *ret_editor = editor;
  *edit_baton = ctx;

  return SVN_NO_ERROR;
}
