/*
 * repos.c: mod_dav_svn repository provider functions for Subversion
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



#include <httpd.h>
#include <http_protocol.h>
#include <http_log.h>
#include <http_core.h>  /* for ap_construct_url */
#include <mod_dav.h>

#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_strings.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_dav.h"
#include "svn_sorts.h"
#include "svn_version.h"
#include "svn_props.h"

#include "dav_svn.h"
#include "mod_dav_svn.h"


struct dav_stream {
  const dav_resource *res;

  /* for reading from the FS */
  svn_stream_t *rstream;

  /* for writing to the FS. we use wstream OR the handler/baton. */
  svn_stream_t *wstream;
  svn_txdelta_window_handler_t delta_handler;
  void *delta_baton;
};

typedef struct {
  ap_filter_t *output;
  apr_pool_t *pool;
} dav_svn_diff_ctx_t;

typedef struct {
  dav_resource res;
  dav_resource_private priv;
} dav_resource_combined;

/* private context for doing a walk */
typedef struct {
  /* the input walk parameters */
  const dav_walk_params *params;

  /* reused as we walk */
  dav_walk_resource wres;

  /* the current resource */
  dav_resource res;             /* wres.resource refers here */
  dav_resource_private info;    /* the info in res */
  svn_stringbuf_t *uri;            /* the uri within res */
  svn_stringbuf_t *repos_path;     /* the repos_path within res */

} dav_svn_walker_context;


static int dav_svn_parse_version_uri(dav_resource_combined *comb,
                                     const char *path,
                                     const char *label,
                                     int use_checked_in)
{
  const char *slash;
  const char *created_rev_str;

  /* format: CREATED_REV/REPOS_PATH */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  comb->res.type = DAV_RESOURCE_TYPE_VERSION;
  comb->res.versioned = TRUE;

  slash = ap_strchr_c(path, '/');
  if (slash == NULL)
    {
      /* http://host.name/repos/$svn/ver/0

         This URL form refers to the root path of the repository.
      */
      created_rev_str = apr_pstrndup(comb->res.pool, path, strlen(path));
      comb->priv.root.rev = SVN_STR_TO_REV(created_rev_str);
      comb->priv.repos_path = "/";
    }
  else if (slash == path)
    {
      /* the CREATED_REV was missing(?)

         ### not sure this can happen, though, because it would imply two
         ### slashes, yet those are cleaned out within get_resource
      */
      return TRUE;
    }
  else
    {
      apr_size_t len = slash - path;

      created_rev_str = apr_pstrndup(comb->res.pool, path, len);
      comb->priv.root.rev = SVN_STR_TO_REV(created_rev_str);
      comb->priv.repos_path = slash;
    }

  /* if the CREATED_REV parsing blew, then propagate it. */
  if (comb->priv.root.rev == SVN_INVALID_REVNUM)
    return TRUE;

  return FALSE;
}

static int dav_svn_parse_history_uri(dav_resource_combined *comb,
                                     const char *path,
                                     const char *label,
                                     int use_checked_in)
{
  /* format: ??? */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  comb->res.type = DAV_RESOURCE_TYPE_HISTORY;

  /* ### parse path */
  comb->priv.repos_path = path;

  return FALSE;
}

static int dav_svn_parse_working_uri(dav_resource_combined *comb,
                                     const char *path,
                                     const char *label,
                                     int use_checked_in)
{
  const char *slash;

  /* format: ACTIVITY_ID/REPOS_PATH */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  comb->res.type = DAV_RESOURCE_TYPE_WORKING;
  comb->res.working = TRUE;
  comb->res.versioned = TRUE;

  slash = ap_strchr_c(path, '/');

  /* This sucker starts with a slash.  That's bogus. */
  if (slash == path)
    return TRUE;

  if (slash == NULL)
    {
      /* There's no slash character in our path.  Assume it's just an
         ACTIVITY_ID pointing to the root path.  That should be cool.
         We'll just drop through to the normal case handling below. */
      comb->priv.root.activity_id = apr_pstrdup(comb->res.pool, path);
      comb->priv.repos_path = "/";
    }
  else
    {
      comb->priv.root.activity_id = apr_pstrndup(comb->res.pool, path,
                                                 slash - path);
      comb->priv.repos_path = slash;
    }

  return FALSE;
}

static int dav_svn_parse_activity_uri(dav_resource_combined *comb,
                                      const char *path,
                                      const char *label,
                                      int use_checked_in)
{
  /* format: ACTIVITY_ID */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  comb->res.type = DAV_RESOURCE_TYPE_ACTIVITY;

  comb->priv.root.activity_id = path;

  return FALSE;
}

static int dav_svn_parse_vcc_uri(dav_resource_combined *comb,
                                 const char *path,
                                 const char *label,
                                 int use_checked_in)
{
  /* format: "default" (a singleton) */

  if (strcmp(path, DAV_SVN_DEFAULT_VCC_NAME) != 0)
    return TRUE;

  if (label == NULL && !use_checked_in)
    {
      /* Version Controlled Configuration (baseline selector) */

      /* ### mod_dav has a proper model for these. technically, they are
         ### version-controlled resources (REGULAR), but that just monkeys
         ### up a lot of stuff for us. use a PRIVATE for now. */

      comb->res.type = DAV_RESOURCE_TYPE_PRIVATE;   /* _REGULAR */
      comb->priv.restype = DAV_SVN_RESTYPE_VCC;

      comb->res.exists = TRUE;
      comb->res.versioned = TRUE;
      comb->res.baselined = TRUE;

      /* NOTE: comb->priv.repos_path == NULL */
    }
  else
    {
      /* a specific Version Resource; in this case, a Baseline */

      int revnum;

      if (label != NULL)
        {
          revnum = SVN_STR_TO_REV(label); /* assume slash terminates */
          if (!SVN_IS_VALID_REVNUM(revnum))
            return TRUE;        /* ### be nice to get better feedback */
        }
      else /* use_checked_in */
        {
          /* use the DAV:checked-in value of the VCC. this is always the
             "latest" (or "youngest") revision. */

          /* signal dav_svn_prep_version to look it up */
          revnum = SVN_INVALID_REVNUM;
        }

      comb->res.type = DAV_RESOURCE_TYPE_VERSION;

      /* exists? need to wait for now */
      comb->res.versioned = TRUE;
      comb->res.baselined = TRUE;

      /* which baseline (revision tree) to access */
      comb->priv.root.rev = revnum;

      /* NOTE: comb->priv.repos_path == NULL */
      /* NOTE: comb->priv.created_rev == SVN_INVALID_REVNUM */
    }

  return FALSE;
}

static int dav_svn_parse_baseline_coll_uri(dav_resource_combined *comb,
                                           const char *path,
                                           const char *label,
                                           int use_checked_in)
{
  const char *slash;
  int revnum;

  /* format: REVISION/REPOS_PATH */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  slash = ap_strchr_c(path, '/');
  if (slash == NULL)
    slash = "/";        /* they are referring to the root of the BC */
  else if (slash == path)
    return TRUE;        /* the REVISION was missing(?)
                           ### not sure this can happen, though, because
                           ### it would imply two slashes, yet those are
                           ### cleaned out within get_resource */

  revnum = SVN_STR_TO_REV(path);  /* assume slash terminates conversion */
  if (!SVN_IS_VALID_REVNUM(revnum))
    return TRUE;        /* ### be nice to get better feedback */

  /* ### mod_dav doesn't have a proper model for these. they are standard
     ### VCRs, but we need some additional semantics attached to them.
     ### need to figure out a way to label them as special. */

  comb->res.type = DAV_RESOURCE_TYPE_REGULAR;
  comb->res.versioned = TRUE;
  comb->priv.root.rev = revnum;
  comb->priv.repos_path = slash;

  return FALSE;
}

static int dav_svn_parse_baseline_uri(dav_resource_combined *comb,
                                      const char *path,
                                      const char *label,
                                      int use_checked_in)
{
  int revnum;

  /* format: REVISION */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  revnum = SVN_STR_TO_REV(path);
  if (!SVN_IS_VALID_REVNUM(revnum))
    return TRUE;        /* ### be nice to get better feedback */

  /* create a Baseline resource (a special Version Resource) */

  comb->res.type = DAV_RESOURCE_TYPE_VERSION;

  /* exists? need to wait for now */
  comb->res.versioned = TRUE;
  comb->res.baselined = TRUE;

  /* which baseline (revision tree) to access */
  comb->priv.root.rev = revnum;

  /* NOTE: comb->priv.repos_path == NULL */
  /* NOTE: comb->priv.created_rev == SVN_INVALID_REVNUM */

  return FALSE;
}

static int dav_svn_parse_wrk_baseline_uri(dav_resource_combined *comb,
                                          const char *path,
                                          const char *label,
                                          int use_checked_in)
{
  const char *slash;

  /* format: ACTIVITY_ID/REVISION */

  /* ### what to do with LABEL and USE_CHECKED_IN ?? */

  comb->res.type = DAV_RESOURCE_TYPE_WORKING;
  comb->res.working = TRUE;
  comb->res.versioned = TRUE;
  comb->res.baselined = TRUE;

  if ((slash = ap_strchr_c(path, '/')) == NULL
      || slash == path
      || slash[1] == '\0')
    return TRUE;

  comb->priv.root.activity_id = apr_pstrndup(comb->res.pool, path,
                                             slash - path);
  comb->priv.root.rev = SVN_STR_TO_REV(slash + 1);

  /* NOTE: comb->priv.repos_path == NULL */

  return FALSE;
}

static const struct special_defn
{
  const char *name;

  /*
   * COMB is the resource that we are constructing. Any elements that
   * can be determined from the PATH may be set in COMB. However, further
   * operations are not allowed (we don't want anything besides a parse
   * error to occur).
   *
   * At a minimum, the parse function must set COMB->res.type and
   * COMB->priv.repos_path.
   *
   * PATH does not contain a leading slash. Given "/root/$svn/xxx/the/path"
   * as the request URI, the PATH variable will be "the/path"
   */
  int (*parse)(dav_resource_combined *comb, const char *path,
               const char *label, int use_checked_in);

  /* The number of subcompenents after the !svn/xxx/... before we
     reach the actual path within the repository. */
  int numcomponents;

  /* Boolean:  are the subcomponents followed by a repos path? */
  int has_repos_path;

  /* The private resource type for the /$svn/xxx/ collection. */
  enum dav_svn_private_restype restype;

} special_subdirs[] =
{
  { "ver", dav_svn_parse_version_uri,
    1, TRUE, DAV_SVN_RESTYPE_VER_COLLECTION },
  { "his", dav_svn_parse_history_uri,
    0, FALSE, DAV_SVN_RESTYPE_HIS_COLLECTION },
  { "wrk", dav_svn_parse_working_uri,
    1, TRUE,  DAV_SVN_RESTYPE_WRK_COLLECTION },
  { "act", dav_svn_parse_activity_uri,
    1, FALSE, DAV_SVN_RESTYPE_ACT_COLLECTION },
  { "vcc", dav_svn_parse_vcc_uri,
    1, FALSE, DAV_SVN_RESTYPE_VCC_COLLECTION },
  { "bc", dav_svn_parse_baseline_coll_uri,
    1, TRUE, DAV_SVN_RESTYPE_BC_COLLECTION },
  { "bln", dav_svn_parse_baseline_uri,
    1, FALSE, DAV_SVN_RESTYPE_BLN_COLLECTION },
  { "wbl", dav_svn_parse_wrk_baseline_uri,
    2, FALSE, DAV_SVN_RESTYPE_WBL_COLLECTION },

  { NULL } /* sentinel */
};

/*
 * dav_svn_parse_uri: parse the provided URI into its various bits
 *
 * URI will contain a path relative to our configured root URI. It should
 * not have a leading "/". The root is identified by "".
 *
 * On output: *COMB will contain all of the information parsed out of
 * the URI -- the resource type, activity ID, path, etc.
 *
 * Note: this function will only parse the URI. Validation of the pieces,
 * opening data stores, etc, are not part of this function.
 *
 * TRUE is returned if a parsing error occurred. FALSE for success.
 */
static int dav_svn_parse_uri(dav_resource_combined *comb,
                             const char *uri,
                             const char *label,
                             int use_checked_in)
{
  const char *special_uri = comb->priv.repos->special_uri;
  apr_size_t len1;
  apr_size_t len2;
  char ch;

  len1 = strlen(uri);
  len2 = strlen(special_uri);
  if (len1 > len2
      && ((ch = uri[len2]) == '/' || ch == '\0')
      && memcmp(uri, special_uri, len2) == 0)
    {
      if (ch == '\0')
        {
          /* URI was "/root/!svn". It exists, but has restricted usage. */
          comb->res.type = DAV_RESOURCE_TYPE_PRIVATE;
          comb->priv.restype = DAV_SVN_RESTYPE_ROOT_COLLECTION;
        }
      else
        {
          const struct special_defn *defn;

          /* skip past the "!svn/" prefix */
          uri += len2 + 1;
          len1 -= len2 + 1;

          for (defn = special_subdirs ; defn->name != NULL; ++defn)
            {
              apr_size_t len3 = strlen(defn->name);

              if (len1 >= len3 && memcmp(uri, defn->name, len3) == 0)
                {
                  if (uri[len3] == '\0')
                    {
                      /* URI was "/root/!svn/XXX". The location exists, but
                         has restricted usage. */
                      comb->res.type = DAV_RESOURCE_TYPE_PRIVATE;

                      /* store the resource type so that we can PROPFIND
                         on this collection. */
                      comb->priv.restype = defn->restype;
                    }
                  else if (uri[len3] == '/')
                    {
                      if ((*defn->parse)(comb, uri + len3 + 1, label,
                                         use_checked_in))
                        return TRUE;
                    }
                  else
                    {
                      /* e.g. "/root/!svn/activity" (we just know "act") */
                      return TRUE;
                    }

                  break;
                }
            }

          /* if completed the loop, then it is an unrecognized subdir */
          if (defn->name == NULL)
            return TRUE;
        }
    }
  else
    {
      /* Anything under the root, but not under "!svn". These are all
         version-controlled resources. */
      comb->res.type = DAV_RESOURCE_TYPE_REGULAR;
      comb->res.versioned = TRUE;

      /* The location of these resources corresponds directly to the URI,
         and we keep the leading "/". */
      comb->priv.repos_path = comb->priv.uri_path->data;
    }

  return FALSE;
}

static dav_error * dav_svn_prep_regular(dav_resource_combined *comb)
{
  apr_pool_t *pool = comb->res.pool;
  dav_svn_repos *repos = comb->priv.repos;
  svn_error_t *serr;
  svn_node_kind_t kind;

  /* A REGULAR resource might have a specific revision already (e.g. if it
     is part of a baseline collection). However, if it doesn't, then we
     will assume that we need the youngest revision.
     ### other cases besides a BC? */
  if (comb->priv.root.rev == SVN_INVALID_REVNUM)
    {
      serr = svn_fs_youngest_rev(&comb->priv.root.rev, repos->fs, pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Could not determine the proper "
                                     "revision to access",
                                     pool);
        }
    }

  /* get the root of the tree */
  serr = svn_fs_revision_root(&comb->priv.root.root, repos->fs,
                              comb->priv.root.rev, pool);
  if (serr != NULL)
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Could not open the root of the "
                                 "repository",
                                 pool);
    }

  serr = svn_fs_check_path(&kind, comb->priv.root.root,
                           comb->priv.repos_path, pool);
  if (serr != NULL)
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 apr_psprintf(pool, "Error checking kind of "
                                              "path '%s' in repository",
                                              comb->priv.repos_path),
                                 pool);
    }

  comb->res.exists = (kind == svn_node_none) ? FALSE : TRUE;
  comb->res.collection = (kind == svn_node_dir) ? TRUE : FALSE;

  return NULL;
}

static dav_error * dav_svn_prep_version(dav_resource_combined *comb)
{
  svn_error_t *serr;
  apr_pool_t *pool = comb->res.pool;

  /* we are accessing the Version Resource by REV/PATH */
  
  /* ### assert: .baselined = TRUE */
  
  /* if we don't have a revision, then assume the youngest */
  if (!SVN_IS_VALID_REVNUM(comb->priv.root.rev))
    {
      serr = svn_fs_youngest_rev(&comb->priv.root.rev,
                                 comb->priv.repos->fs,
                                 pool);
      if (serr != NULL)
        {
          /* ### might not be a baseline */
          
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Could not fetch 'youngest' revision "
                                     "to enable accessing the latest "
                                     "baseline resource.",
                                     pool);
        }
    }
  
  /* ### baselines have no repos_path, and we don't need to open
     ### a root (yet). we just needed to ensure that we have the proper
     ### revision number. */

  if (!comb->priv.root.root)
    {
      serr = svn_fs_revision_root(&comb->priv.root.root, 
                                  comb->priv.repos->fs,
                                  comb->priv.root.rev,
                                  pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Could not open a revision root.",
                                     pool);
        }
    }

  /* ### we should probably check that the revision is valid */
  comb->res.exists = TRUE;
  
  /* Set up the proper URI. Most likely, we arrived here via a VCC,
     so the URI will be incorrect. Set the canonical form. */
  /* ### assuming a baseline */
  comb->res.uri = dav_svn_build_uri(comb->priv.repos,
                                    DAV_SVN_BUILD_URI_BASELINE,
                                    comb->priv.root.rev, NULL,
                                    0 /* add_href */,
                                    pool);

  return NULL;
}

static dav_error * dav_svn_prep_history(dav_resource_combined *comb)
{
  return NULL;
}

static dav_error * dav_svn_prep_working(dav_resource_combined *comb)
{
  const char *txn_name = dav_svn_get_txn(comb->priv.repos,
                                         comb->priv.root.activity_id);
  apr_pool_t *pool = comb->res.pool;
  svn_error_t *serr;
  svn_node_kind_t kind;

  if (txn_name == NULL)
    {
      /* ### HTTP_BAD_REQUEST is probably wrong */
      return dav_new_error(pool, HTTP_BAD_REQUEST, 0,
                           "An unknown activity was specified in the URL. "
                           "This is generally caused by a problem in the "
                           "client software.");
    }
  comb->priv.root.txn_name = txn_name;

  /* get the FS transaction, given its name */
  serr = svn_fs_open_txn(&comb->priv.root.txn, comb->priv.repos->fs, txn_name,
                         pool);
  if (serr != NULL)
    {
      if (serr->apr_err == SVN_ERR_FS_NO_SUCH_TRANSACTION)
        {
          svn_error_clear(serr);
          return dav_new_error(pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                               "An activity was specified and found, but the "
                               "corresponding SVN FS transaction was not "
                               "found.");
        }
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Could not open the SVN FS transaction "
                                 "corresponding to the specified activity.",
                                 pool);
    }

  if (comb->res.baselined)
    {
      /* a Working Baseline */

      /* if the transaction exists, then the working resource exists */
      comb->res.exists = TRUE;

      return NULL;
    }

  /* Set the txn author if not previously set.  Protect against multi-author
   * commits by verifying authenticated user associated with the current
   * request is the same as the txn author.
   * Note that anonymous requests are being excluded as being a change
   * in author, because the commit may touch areas of the repository
   * that are anonymous writeable as well as areas that are not.
   */
  if (comb->priv.repos->username)
    {
      svn_string_t *current_author;
      svn_string_t request_author;

      serr = svn_fs_txn_prop(&current_author, comb->priv.root.txn,
               SVN_PROP_REVISION_AUTHOR, pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                   "Failed to retrieve author of the SVN FS transaction "
                   "corresponding to the specified activity.",
                   pool);
        }

      request_author.data = comb->priv.repos->username;
      request_author.len = strlen(request_author.data);
      if (!current_author)
        {
          serr = svn_fs_change_txn_prop(comb->priv.root.txn,
                   SVN_PROP_REVISION_AUTHOR, &request_author, pool);
          if (serr != NULL)
            {
              return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                       "Failed to set the author of the SVN FS transaction "
                       "corresponding to the specified activity.",
                       pool);
            }
        }
      else if (!svn_string_compare(current_author, &request_author))
        {
          return dav_new_error(pool, HTTP_NOT_IMPLEMENTED, 0,
                   "Multi-author commits not supported.");
        }
    }

  /* get the root of the tree */
  serr = svn_fs_txn_root(&comb->priv.root.root, comb->priv.root.txn, pool);
  if (serr != NULL)
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Could not open the (transaction) root of "
                                 "the repository",
                                 pool);
    }

  serr = svn_fs_check_path(&kind, comb->priv.root.root,
                           comb->priv.repos_path, pool);
  if (serr != NULL)
    {
      return dav_svn_convert_err
        (serr, HTTP_INTERNAL_SERVER_ERROR,
         apr_psprintf(pool, "Error checking kind of path '%s' in repository",
                      comb->priv.repos_path),
         pool);
    }

  comb->res.exists = (kind == svn_node_none) ? FALSE : TRUE;
  comb->res.collection = (kind == svn_node_dir) ? TRUE : FALSE;

  return NULL;
}

static dav_error * dav_svn_prep_activity(dav_resource_combined *comb)
{
  const char *txn_name = dav_svn_get_txn(comb->priv.repos,
                                         comb->priv.root.activity_id);

  comb->priv.root.txn_name = txn_name;
  comb->res.exists = txn_name != NULL;

  return NULL;
}

static dav_error * dav_svn_prep_private(dav_resource_combined *comb)
{
  if (comb->priv.restype == DAV_SVN_RESTYPE_VCC)
    {
      /* ### what to do */
    }
  /* else nothing to do (### for now) */

  return NULL;
}

static const struct res_type_handler
{
  dav_resource_type type;
  dav_error * (*prep)(dav_resource_combined *comb);

} res_type_handlers[] =
{
  /* skip UNKNOWN */
  { DAV_RESOURCE_TYPE_REGULAR, dav_svn_prep_regular },
  { DAV_RESOURCE_TYPE_VERSION, dav_svn_prep_version },
  { DAV_RESOURCE_TYPE_HISTORY, dav_svn_prep_history },
  { DAV_RESOURCE_TYPE_WORKING, dav_svn_prep_working },
  /* skip WORKSPACE */
  { DAV_RESOURCE_TYPE_ACTIVITY, dav_svn_prep_activity },
  { DAV_RESOURCE_TYPE_PRIVATE, dav_svn_prep_private },

  { 0, NULL }   /* sentinel */
};

/*
** ### docco...
**
** Set .exists and .collection
** open other, internal bits...
*/
static dav_error * dav_svn_prep_resource(dav_resource_combined *comb)
{
  const struct res_type_handler *scan;

  for (scan = res_type_handlers; scan->prep != NULL; ++scan)
    {
      if (comb->res.type == scan->type)
        return (*scan->prep)(comb);
    }

  return dav_new_error(comb->res.pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                       "DESIGN FAILURE: unknown resource type");
}

static dav_resource *dav_svn_create_private_resource(
    const dav_resource *base,
    enum dav_svn_private_restype restype)
{
  dav_resource_combined *comb;
  svn_stringbuf_t *path;
  const struct special_defn *defn;

  for (defn = special_subdirs; defn->name != NULL; ++defn)
    if (defn->restype == restype)
      break;
  /* assert: defn->name != NULL */

  path = svn_stringbuf_createf(base->pool, "/%s/%s",
                            base->info->repos->special_uri, defn->name);

  comb = apr_pcalloc(base->pool, sizeof(*comb));

  /* ### can/should we leverage dav_svn_prep_resource */

  comb->res.type = DAV_RESOURCE_TYPE_PRIVATE;

  comb->res.exists = TRUE;
  comb->res.collection = TRUE;                  /* ### always true? */
  /* versioned = baselined = working = FALSE */

  comb->res.uri = apr_pstrcat(base->pool, base->info->repos->root_path,
                              path->data, NULL);
  comb->res.info = &comb->priv;
  comb->res.hooks = &dav_svn_hooks_repos;
  comb->res.pool = base->pool;

  comb->priv.uri_path = path;
  comb->priv.repos = base->info->repos;
  comb->priv.root.rev = SVN_INVALID_REVNUM;
  return &comb->res;
}

static void log_warning(void *baton, svn_error_t *err)
{
  request_rec *r = baton;

  /* ### hmm. the FS is cleaned up at request cleanup time. "r" might
     ### not really be valid. we should probably put the FS into a
     ### subpool to ensure it gets cleaned before the request.

     ### is there a good way to create and use a subpool for all
     ### of our functions ... ??
  */

  ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_EGENERAL, r, "%s", err->message);
}


AP_MODULE_DECLARE(dav_error *)
dav_svn_split_uri(request_rec *r,
                  const char *uri_to_split,
                  const char *root_path,
                  const char **cleaned_uri,
                  int *trailing_slash,
                  const char **repos_name,
                  const char **relative_path,
                  const char **repos_path)
{
  apr_size_t len1;
  int had_slash;
  const char *fs_path;
  const char *fs_parent_path;
  const char *relative;
  char *uri;

  /* one of these is NULL, the other non-NULL. */
  fs_path = dav_svn_get_fs_path(r);
  fs_parent_path = dav_svn_get_fs_parent_path(r);

  if ((fs_path == NULL) && (fs_parent_path == NULL))
    {
      /* ### are SVN_ERR_APMOD codes within the right numeric space? */
      return dav_new_error(r->pool, HTTP_INTERNAL_SERVER_ERROR,
                           SVN_ERR_APMOD_MISSING_PATH_TO_FS,
                           "The server is misconfigured: "
                           "either an SVNPath or SVNParentPath "
                           "directive is required to specify the location "
                           "of this resource's repository.");
    }

  /* make a copy so that we can do some work on it */
  uri = apr_pstrdup(r->pool, uri_to_split);

  /* remove duplicate slashes, and make sure URI has no trailing '/' */
  ap_no2slash(uri);
  len1 = strlen(uri);
  had_slash = (len1 > 0 && uri[len1 - 1] == '/');
  if (len1 > 1 && had_slash)
    uri[len1 - 1] = '\0';
  
  if (had_slash)
    *trailing_slash = TRUE;
  else
    *trailing_slash = FALSE;

  /* return the first item.  */
  *cleaned_uri = apr_pstrdup(r->pool, uri);

  /* The URL space defined by the SVN provider is always a virtual
     space. Construct the path relative to the configured Location
     (root_path). So... the relative location is simply the URL used,
     skipping the root_path.
     
     Note: mod_dav has canonialized root_path. It will not have a trailing
     slash (unless it is "/").

     Note: given a URI of /something and a root of /some, then it is
           impossible to be here (and end up with "thing"). This is simply
           because we control /some and are dispatched to here for its
           URIs. We do not control /something, so we don't get here. Or,
           if we *do* control /something, then it is for THAT root.
  */
  relative = ap_stripprefix(uri, root_path);

  /* We want a leading slash on the path specified by <relative>. This
     will almost always be the case since root_path does not have a trailing
     slash. However, if the root is "/", then the slash will be removed
     from <relative>. Backing up a character will put the leading slash
     back.

     Watch out for the empty string! This can happen when URI == ROOT_PATH.
     We simply turn the path into "/" for this case. */
  if (*relative == '\0')
    relative = "/";
  else if (*relative != '/')
    --relative;
  /* ### need a better name... it isn't "relative" because of the leading
     ### slash. something about SVN-private-path */

  /* Depending on whether SVNPath or SVNParentPath was used, we need
     to compute 'relative' and 'repos_name' differently.  */

  /* Normal case:  the SVNPath command was used to specify a
     particular repository.  */
  if (fs_path != NULL)
    {
      /* the repos_name is the last component of root_path. */
      *repos_name = svn_path_basename(root_path, r->pool); 

      /* 'relative' is already correct for SVNPath; the root_path
         already contains the name of the repository, so relative is
         everything beyond that.  */      
    }

  else
    {
      /* SVNParentPath was used instead: assume the first component of
         'relative' is the name of a repository. */
      const char *magic_component, *magic_end;

      /* A repository name is required here.
         Remember that 'relative' always starts with a "/". */
      if (relative[1] == '\0')
        {
          /* ### are SVN_ERR_APMOD codes within the right numeric space? */
          return dav_new_error(r->pool, HTTP_FORBIDDEN,
                               SVN_ERR_APMOD_MALFORMED_URI,
                               "The URI does not contain the name "
                               "of a repository.");
        }

      magic_end = ap_strchr_c(relative + 1, '/');
      if (!magic_end)
        {
          /* ### Request was for parent directory with no trailing
             slash; we probably ought to just redirect to same with
             trailing slash appended. */
          magic_component = relative + 1;
          relative = "/";
        }
      else
        {
          magic_component = apr_pstrndup(r->pool, relative + 1,
                                         magic_end - relative - 1);
          relative = magic_end;
        }

      /* return answer */
      *repos_name = magic_component;
    }

  /* We can return 'relative' at this point too. */
  *relative_path = apr_pstrdup(r->pool, relative);

  /* Code to remove the !svn junk from the front of the relative path,
     mainly stolen from dav_svn_parse_uri().  This code assumes that
     the 'relative' string being parsed doesn't start with '/'. */
  relative++;

  {
    const char *special_uri = dav_svn_get_special_uri(r);
    apr_size_t len2;
    char ch;

    len1 = strlen(relative);
    len2 = strlen(special_uri);
    if (len1 > len2
        && ((ch = relative[len2]) == '/' || ch == '\0')
        && memcmp(relative, special_uri, len2) == 0)
      {
        if (ch == '\0')
          {
            /* relative is just "!svn", which is malformed. */
            return dav_new_error(r->pool, HTTP_INTERNAL_SERVER_ERROR,
                                 SVN_ERR_APMOD_MALFORMED_URI,
                                 "Nothing follows the svn special_uri.");
          }
        else
          {
            const struct special_defn *defn;
            
            /* skip past the "!svn/" prefix */
            relative += len2 + 1;
            len1 -= len2 + 1;
            
            for (defn = special_subdirs ; defn->name != NULL; ++defn)
              {
                apr_size_t len3 = strlen(defn->name);
                
                if (len1 >= len3 && memcmp(relative, defn->name, len3) == 0)
                  {
                    /* Found a matching special dir. */

                    if (relative[len3] == '\0')
                      {
                        /* relative is "!svn/xxx"  */
                        if (defn->numcomponents == 0)
                          *repos_path = NULL;
                        else
                          return 
                            dav_new_error(r->pool, HTTP_INTERNAL_SERVER_ERROR,
                                          SVN_ERR_APMOD_MALFORMED_URI,
                                          "Missing info after special_uri.");
                      }
                    else if (relative[len3] == '/')
                      {                      
                        /* Skip past defn->numcomponents components,
                           return everything beyond that.*/
                        int j;
                        const char *end = NULL, *start = relative + len3 + 1;
                        
                        for (j = 0; j < defn->numcomponents; j++)
                          {
                            end = ap_strchr_c(start, '/');
                            if (! end)
                              break;
                            start = end + 1;
                          }

                        if (! end)
                          {
                            /* Did we break from the loop prematurely? */
                            if (j != (defn->numcomponents - 1))
                              return 
                                dav_new_error(r->pool,
                                              HTTP_INTERNAL_SERVER_ERROR,
                                              SVN_ERR_APMOD_MALFORMED_URI,
                                              "Not enough components"
                                              " after special_uri.");
                            
                            if (! defn->has_repos_path)
                              /* It's okay to not have found a slash. */
                              *repos_path = NULL;
                            else
                              *repos_path = "/";
                          }
                        else
                          {
                            /* Found a slash after the special components. */
                            *repos_path = apr_pstrdup(r->pool, start);
                          }
                      }
                    else
                      {
                        return
                          dav_new_error(r->pool, HTTP_INTERNAL_SERVER_ERROR,
                                        SVN_ERR_APMOD_MALFORMED_URI,
                                        "Unknown data after special_uri.");
                      }
                    
                  break;
                  }
              }
            
            if (defn->name == NULL)
              return
                dav_new_error(r->pool, HTTP_INTERNAL_SERVER_ERROR,
                              SVN_ERR_APMOD_MALFORMED_URI,
                              "Couldn't match subdir after special_uri.");
          }
      }
    else
      {
        /* There's no "!svn/" at all, so the relative path is already
           a valid path within the repository.  */
        *repos_path = apr_pstrdup(r->pool, relative);
      }    
  }

  return NULL;
}


static dav_error * dav_svn_get_resource(request_rec *r,
                                        const char *root_path,
                                        const char *label,
                                        int use_checked_in,
                                        dav_resource **resource)
{
  const char *fs_path;
  const char *repo_name;
  const char *xslt_uri;
  const char *fs_parent_path;
  dav_resource_combined *comb;
  dav_svn_repos *repos;
  const char *cleaned_uri;
  const char *repos_name;
  const char *relative;
  const char *repos_path;
  const char *repos_key;
  const char *version_name;
  svn_error_t *serr;
  dav_error *err;
  int had_slash;

  repo_name = dav_svn_get_repo_name(r);
  xslt_uri = dav_svn_get_xslt_uri(r);

  /* This does all the work of interpreting/splitting the request uri. */
  err = dav_svn_split_uri(r, r->uri, root_path,
                          &cleaned_uri, &had_slash, 
                          &repos_name, &relative, &repos_path);
  if (err)
    return err;

  /* The path that we will eventually try to open as an svn
     repository.  Normally defined by the SVNPath directive. */
  fs_path = dav_svn_get_fs_path(r);

  /* If the SVNParentPath directive was used instead... */
  fs_parent_path = dav_svn_get_fs_parent_path(r);
  if (fs_parent_path != NULL)
    {      
      /* ...then the URL to the repository is actually one implicit
         component longer... */
      root_path = svn_path_join(root_path, repos_name, r->pool);
      /* ...and we need to specify exactly what repository to open. */
      fs_path = svn_path_join(fs_parent_path, repos_name, r->pool);
    }

  /* Start building and filling a 'combination' object. */
  comb = apr_pcalloc(r->pool, sizeof(*comb));
  comb->res.info = &comb->priv;
  comb->res.hooks = &dav_svn_hooks_repos;
  comb->res.pool = r->pool;
  comb->res.uri = cleaned_uri;

  /* Original request, off which to generate subrequests later. */
  comb->priv.r = r;

  /* ### ugly hack to carry over Content-Type data to the open_stream, which
     ### does not have access to the request headers. */
  {
    const char *ct = apr_table_get(r->headers_in, "content-type");

    comb->priv.is_svndiff =
      ct != NULL
      && strcmp(ct, SVN_SVNDIFF_MIME_TYPE) == 0;
  }

  /* ### and another hack for computing diffs to send to the client */
  comb->priv.delta_base = apr_table_get(r->headers_in,
                                        SVN_DAV_DELTA_BASE_HEADER);

  /* Gather any options requested by an svn client. */
  comb->priv.svn_client_options = apr_table_get(r->headers_in,
                                                SVN_DAV_OPTIONS_HEADER);

  /* See if the client sent a custom 'version name' request header. */
  version_name = apr_table_get(r->headers_in, SVN_DAV_VERSION_NAME_HEADER);
  comb->priv.version_name 
    = version_name ? SVN_STR_TO_REV(version_name): SVN_INVALID_REVNUM;

  /* Remember checksums, if any. */
  comb->priv.base_checksum =
    apr_table_get(r->headers_in, SVN_DAV_BASE_FULLTEXT_MD5_HEADER);
  comb->priv.result_checksum =
    apr_table_get(r->headers_in, SVN_DAV_RESULT_FULLTEXT_MD5_HEADER);

  /* "relative" is part of the "uri" string, so it has the proper
     lifetime to store here. */
  /* ### that comment no longer applies. we're creating a string with its
     ### own lifetime now. so WHY are we using a string? hmm... */
  comb->priv.uri_path = svn_stringbuf_create(relative, r->pool);

  /* initialize this until we put something real here */
  comb->priv.root.rev = SVN_INVALID_REVNUM;

  /* create the repository structure and stash it away */
  repos = apr_pcalloc(r->pool, sizeof(*repos));
  repos->pool = r->pool;

  comb->priv.repos = repos;

  /* We are assuming the root_path will live at least as long as this
     resource. Considering that it typically comes from the per-dir
     config in mod_dav, this is valid for now. */
  repos->root_path = svn_path_uri_encode(root_path, r->pool);

  /* where is the SVN FS for this resource? */
  repos->fs_path = fs_path;

  /* A name for the repository */
  repos->repo_name = repo_name;

  /* An XSL transformation */
  repos->xslt_uri = xslt_uri;

  /* Is autoversioning active in this repos? */
  repos->autoversioning = dav_svn_get_autoversioning_flag(r);

  /* Remember various bits for later URL construction */
  repos->base_url = ap_construct_url(r->pool, "", r);
  repos->special_uri = dav_svn_get_special_uri(r);

  /* Remember who is making this request */
  repos->username = r->user;

  /* Retrieve/cache open repository */
  repos_key = apr_pstrcat(r->pool, "mod_dav_svn:", fs_path, NULL);
  apr_pool_userdata_get((void **)&repos->repos, repos_key, r->connection->pool);
  if (repos->repos == NULL)
    {
      serr = svn_repos_open(&(repos->repos), fs_path, r->connection->pool);
      if (serr != NULL)
        {
          /* The error returned by svn_repos_open might contain the
             actual path to the failed repository.  We don't want to
             leak that path back to the client, because that would be
             a security risk, but we do want to log the real error on
             the server side. */
          const char *new_msg = "Could not open the requested SVN filesystem";
          svn_error_t *sanitized_error = svn_error_create(serr->apr_err,
                                                          NULL, new_msg);

          ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_EGENERAL, r,
                        "%s", serr->message);

          /* Return a slightly less informative error to dav. */
          svn_error_clear(serr);
          return dav_svn_convert_err(sanitized_error,
                                     HTTP_INTERNAL_SERVER_ERROR,
                                     apr_psprintf(r->pool, new_msg),
                                     r->pool);
        }

      /* Cache the open repos for the next request on this connection */
      apr_pool_userdata_set(repos->repos, repos_key,
                            NULL, r->connection->pool);
    }

  /* cache the filesystem object */
  repos->fs = svn_repos_fs(repos->repos);

  /* capture warnings during cleanup of the FS */
  svn_fs_set_warning_func(repos->fs, log_warning, r);

  /* Figure out the type of the resource. Note that we have a PARSE step
     which is separate from a PREP step. This is because the PARSE can
     map multiple URLs to the same resource type. The PREP operates on
     the type of the resource. */

  /* skip over the leading "/" in the relative URI */
  if (dav_svn_parse_uri(comb, relative + 1, label, use_checked_in))
    goto malformed_URI;

#ifdef SVN_DEBUG
  if (comb->res.type == DAV_RESOURCE_TYPE_UNKNOWN)
    {
      /* Unknown URI. Return NULL to indicate "no resource" */
      DBG0("DESIGN FAILURE: should not be UNKNOWN at this point");
      *resource = NULL;
      return NULL;
    }
#endif

  /* prepare the resource for operation */
  if ((err = dav_svn_prep_resource(comb)) != NULL)
    return err;

  /* a GET request for a REGULAR collection resource MUST have a trailing
     slash. Redirect to include one if it does not. */
  if (comb->res.collection && comb->res.type == DAV_RESOURCE_TYPE_REGULAR
      && !had_slash && r->method_number == M_GET)
    {
      /* note that we drop r->args. we don't deal with them anyways */
      const char *new_path = apr_pstrcat(r->pool,
                                         ap_escape_uri(r->pool, r->uri),
                                         "/",
                                         NULL);
      apr_table_setn(r->headers_out, "Location",
                     ap_construct_url(r->pool, new_path, r));
      return dav_new_error(r->pool, HTTP_MOVED_PERMANENTLY, 0,
                           "Requests for a collection must have a "
                           "trailing slash on the URI.");
    }

  *resource = &comb->res;
  return NULL;

 malformed_URI:
  /* A malformed URI error occurs when a URI indicates the "special" area,
     yet it has an improper construction. Generally, this is because some
     doofus typed it in manually or has a buggy client. */
  /* ### pick something other than HTTP_INTERNAL_SERVER_ERROR */
  /* ### are SVN_ERR_APMOD codes within the right numeric space? */
  return dav_new_error(r->pool, HTTP_INTERNAL_SERVER_ERROR,
                       SVN_ERR_APMOD_MALFORMED_URI,
                       "The URI indicated a resource within Subversion's "
                       "special resource area, but does not exist. This is "
                       "generally caused by a problem in the client "
                       "software.");
}

static dav_error * dav_svn_get_parent_resource(const dav_resource *resource,
                                               dav_resource **parent_resource)
{
  svn_stringbuf_t *path = resource->info->uri_path;

  /* the root of the repository has no parent */
  if (path->len == 1 && *path->data == '/')
    {
      *parent_resource = NULL;
      return NULL;
    }

  switch (resource->type)
    {
    case DAV_RESOURCE_TYPE_WORKING:
    case DAV_RESOURCE_TYPE_REGULAR:
      /* The "/" occurring within the URL of working resources is part of
         its identifier; it does not establish parent resource relationships.
         All working resources have the same parent, which is:
         http://host.name/path2repos/$svn/wrk/
      */
      *parent_resource =
        dav_svn_create_private_resource(resource,
                                        DAV_SVN_RESTYPE_WRK_COLLECTION);
      break;

    case DAV_RESOURCE_TYPE_ACTIVITY:
      *parent_resource =
        dav_svn_create_private_resource(resource,
                                        DAV_SVN_RESTYPE_ACT_COLLECTION);
      break;

    default:
      /* ### needs more work. need parents for other resource types
         ###
         ### return an error so we can easily identify the cases where
         ### we've called this function unexpectedly. */
      return dav_new_error(resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                           apr_psprintf(resource->pool,
                                        "get_parent_resource was called for "
                                        "%s (type %d)",
                                        resource->uri, resource->type));
      break;
    }

  return NULL;
}

/* does RES2 live in the same repository as RES1? */
static int is_our_resource(const dav_resource *res1,
                           const dav_resource *res2)
{
  if (res1->hooks != res2->hooks
      || strcmp(res1->info->repos->fs_path, res2->info->repos->fs_path) != 0)
    {
      /* a different provider, or a different FS repository */
      return 0;
    }

  /* coalesce the repository */
  if (res1->info->repos != res2->info->repos)
    {      
      /* ### might be nice to have a pool which we can clear to toss
         ### out the old, redundant repos/fs.  */

      /* have res2 point to res1's filesystem */
      res2->info->repos = res1->info->repos;

      /* res2's fs_root object is now invalid.  regenerate it using
         the now-shared filesystem. */
      if (res2->info->root.txn_name)
        {
          /* reopen the txn by name */
          svn_error_clear(svn_fs_open_txn(&(res2->info->root.txn),
                                          res2->info->repos->fs,
                                          res2->info->root.txn_name,
                                          res2->info->repos->pool));

          /* regenerate the txn "root" object */
          svn_error_clear(svn_fs_txn_root(&(res2->info->root.root),
                                          res2->info->root.txn,
                                          res2->info->repos->pool));
        }
      else if (res2->info->root.rev)
        {
          /* default:  regenerate the revision "root" object */
          svn_error_clear(svn_fs_revision_root(&(res2->info->root.root),
                                               res2->info->repos->fs,
                                               res2->info->root.rev,
                                               res2->info->repos->pool));
        }
    }

  return 1;
}

static int dav_svn_is_same_resource(const dav_resource *res1,
                                    const dav_resource *res2)
{
  if (!is_our_resource(res1, res2))
    return 0;

  /* ### what if the same resource were reached via two URIs? */

  return svn_stringbuf_compare(res1->info->uri_path, res2->info->uri_path);
}

static int dav_svn_is_parent_resource(const dav_resource *res1,
                                      const dav_resource *res2)
{
  apr_size_t len1 = strlen(res1->info->uri_path->data);
  apr_size_t len2;

  if (!is_our_resource(res1, res2))
    return 0;

  /* ### what if a resource were reached via two URIs? we ought to define
     ### parent/child relations for resources independent of URIs.
     ### i.e. define a "canonical" location for each resource, then return
     ### the parent based on that location. */

  /* res2 is one of our resources, we can use its ->info ptr */
  len2 = strlen(res2->info->uri_path->data);

  return (len2 > len1
          && memcmp(res1->info->uri_path->data, res2->info->uri_path->data,
                    len1) == 0
          && res2->info->uri_path->data[len1] == '/');
}


dav_error * dav_svn_resource_kind(request_rec *r,
                                  const char *uri,
                                  const char *root_path,
                                  svn_node_kind_t *kind)
{
  dav_error *derr;
  svn_error_t *serr;
  dav_resource *resource;
  svn_revnum_t base_rev;
  svn_fs_root_t *base_rev_root;
  char *saved_uri;

  /* Temporarily insert the uri that the user actually wants us to
     convert into a resource.  Typically, this is already r->uri, so
     this is usually a no-op.  But sometimes the caller may pass in
     the Destination: header uri.

     ### WHAT WE REALLY WANT here is to refactor dav_svn_get_resource,
     so that some alternate interface actually allows us to specify
     the URI to process, i.e. not always process r->uri.
  */
  saved_uri = r->uri;
  r->uri = apr_pstrdup(r->pool, uri);
 
  /* parse the uri and prep the associated resource. */
  derr = dav_svn_get_resource(r, root_path,
                              /* ### I can't believe that every single
                                 parser ignores the LABEL and USE_CHECKED_IN
                                 args below!! */
                              "ignored_label", 1,
                              &resource);
  /* Restore r back to normal. */
  r->uri = saved_uri;
  
  if (derr)
    return derr;

  if (resource->type == DAV_RESOURCE_TYPE_REGULAR)
    {
      /* Either a public URI or a bc.  In both cases, prep_regular()
         has already set the 'exists' and 'collection' flags by
         querying the appropriate revision root and path.  */
      if (! resource->exists)
        *kind = svn_node_none;
      else
        *kind = resource->collection ? svn_node_dir : svn_node_file;
    }

  else if (resource->type == DAV_RESOURCE_TYPE_VERSION)
    {
      if (resource->baselined)  /* bln */
        *kind = svn_node_unknown;

      else /* ver */
        {
          serr = svn_fs_check_path(kind, resource->info->root.root,
                                   resource->info->repos_path, r->pool);

          if (serr)
            return dav_svn_convert_err
              (serr, HTTP_INTERNAL_SERVER_ERROR,
               apr_psprintf(r->pool, 
                            "Error checking kind of path '%s' in repository", 
                            resource->info->repos_path),
               r->pool);
        }
    }
  
  else if (resource->type == DAV_RESOURCE_TYPE_WORKING)
    {
      if (resource->baselined) /* wbl */
        *kind = svn_node_unknown;
      
      else /* wrk */
        {
          /* don't call fs_check_path on the txn, but on the original
             revision that the txn is based on. */
          base_rev = svn_fs_txn_base_revision(resource->info->root.txn);
          serr = svn_fs_revision_root(&base_rev_root,
                                      resource->info->repos->fs,
                                      base_rev, r->pool);
          if (serr)
            return dav_svn_convert_err
              (serr, HTTP_INTERNAL_SERVER_ERROR,
               apr_psprintf(r->pool,
                            "Could not open root of revision %ld",
                            base_rev),
               r->pool);
      
          serr = svn_fs_check_path(kind, base_rev_root,
                                   resource->info->repos_path, r->pool);
          if (serr)
            return dav_svn_convert_err
              (serr, HTTP_INTERNAL_SERVER_ERROR,
               apr_psprintf(r->pool,
                            "Error checking kind of path '%s' in repository",
                            resource->info->repos_path),
               r->pool);
        }
    }

  else
    /* act, his, vcc, or some other private resource */
    *kind = svn_node_unknown;

  return NULL;
}


static dav_error * dav_svn_open_stream(const dav_resource *resource,
                                       dav_stream_mode mode,
                                       dav_stream **stream)
{
  svn_error_t *serr;

  if (mode == DAV_MODE_WRITE_TRUNC || mode == DAV_MODE_WRITE_SEEKABLE)
    {
      if (resource->type != DAV_RESOURCE_TYPE_WORKING)
        {
          return dav_new_error(resource->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                               "Resource body changes may only be made to "
                               "working resources [at this time].");
        }
    }

#if 1
  if (mode == DAV_MODE_WRITE_SEEKABLE)
    {
      return dav_new_error(resource->pool, HTTP_NOT_IMPLEMENTED, 0,
                           "Resource body writes cannot use ranges "
                           "[at this time].");
    }
#endif

  /* start building the stream structure */
  *stream = apr_pcalloc(resource->pool, sizeof(**stream));
  (*stream)->res = resource;

  /* note: when writing, we don't need to use DAV_SVN_REPOS_PATH since
     we cannot write into an "id root". Partly because the FS may not
     let us, but mostly that we have an id root only to deal with Version
     Resources, and those are read only. */

  serr = svn_fs_apply_textdelta(&(*stream)->delta_handler,
                                &(*stream)->delta_baton,
                                resource->info->root.root,
                                resource->info->repos_path,
                                resource->info->base_checksum,
                                resource->info->result_checksum,
                                resource->pool);
  if (serr != NULL && serr->apr_err == SVN_ERR_FS_NOT_FOUND)
    {
      svn_error_clear(serr);
      serr = svn_fs_make_file(resource->info->root.root,
                              resource->info->repos_path,
                              resource->pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Could not create file within the "
                                     "repository.",
                                     resource->pool);
        }
      serr = svn_fs_apply_textdelta(&(*stream)->delta_handler,
                                    &(*stream)->delta_baton,
                                    resource->info->root.root,
                                    resource->info->repos_path,
                                    resource->info->base_checksum,
                                    resource->info->result_checksum,
                                    resource->pool);
    }
  if (serr != NULL)
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Could not prepare to write the file",
                                 resource->pool);
    }

  /* if the incoming data is an SVNDIFF, then create a stream that
     will process the data into windows and invoke the FS window handler
     when a window is ready. */
  /* ### we need a better way to check the content-type! this is bogus
     ### because we're effectively looking at the request_rec. doubly
     ### bogus because this means you cannot open arbitrary streams and
     ### feed them content (the type is always tied to a request_rec).
     ### probably ought to pass the type to open_stream */
  if (resource->info->is_svndiff)
    {
      (*stream)->wstream =
        svn_txdelta_parse_svndiff((*stream)->delta_handler,
                                  (*stream)->delta_baton,
                                  TRUE,
                                  resource->pool);
    }

  return NULL;
}

static dav_error * dav_svn_close_stream(dav_stream *stream, int commit)
{
  svn_error_t *serr;
  apr_pool_t *pool = stream->res->pool;

  if (stream->rstream != NULL)
    {
      serr = svn_stream_close(stream->rstream);
      if (serr)
        return dav_svn_convert_err
          (serr, HTTP_INTERNAL_SERVER_ERROR,
           "dav_svn_close_stream: error closing read stream",
           pool);
    }

  /* if we have a write-stream, then closing it also takes care of the
     handler (so make sure not to send a NULL to it, too) */
  if (stream->wstream != NULL)
    {
      serr = svn_stream_close(stream->wstream);
      if (serr)
        return dav_svn_convert_err
          (serr, HTTP_INTERNAL_SERVER_ERROR,
           "dav_svn_close_stream: error closing write stream",
           pool);
    }
  else if (stream->delta_handler != NULL)
    {
      serr = (*stream->delta_handler)(NULL, stream->delta_baton);
      if (serr)
        return dav_svn_convert_err
          (serr, HTTP_INTERNAL_SERVER_ERROR,
           "dav_svn_close_stream: error sending final (null) delta window",
           pool);
    }

  return NULL;
}

static dav_error * dav_svn_write_stream(dav_stream *stream, const void *buf,
                                        apr_size_t bufsize)
{
  svn_error_t *serr;
  apr_pool_t *pool = stream->res->pool;

  if (stream->wstream != NULL)
    {
      serr = svn_stream_write(stream->wstream, buf, &bufsize);
      /* ### would the returned bufsize ever not match the requested amt? */
    }
  else
    {
      svn_txdelta_window_t window = { 0 };
      svn_txdelta_op_t op;
      svn_string_t data;

      data.data = buf;
      data.len = bufsize;

      op.action_code = svn_txdelta_new;
      op.offset = 0;
      op.length = bufsize;

      window.tview_len = bufsize;   /* result will be this long */
      window.num_ops = 1;
      window.ops = &op;
      window.new_data = &data;

      serr = (*stream->delta_handler)(&window, stream->delta_baton);
    }

  if (serr)
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "could not write the file contents",
                                 pool);
    }
  return NULL;
}

static dav_error * dav_svn_seek_stream(dav_stream *stream,
                                       apr_off_t abs_position)
{
  /* ### fill this in */

  return dav_new_error(stream->res->pool, HTTP_NOT_IMPLEMENTED, 0,
                       "Resource body read/write cannot use ranges "
                       "[at this time].");
}

const char * dav_svn_getetag(const dav_resource *resource, apr_pool_t *pool)
{
  svn_error_t *serr;
  svn_revnum_t created_rev;

  /* if the resource doesn't exist, isn't a simple REGULAR or VERSION
     resource, or it is a Baseline, then it has no etag. */
  /* ### we should assign etags to all resources at some point */
  if (!resource->exists
      || (resource->type != DAV_RESOURCE_TYPE_REGULAR
          && resource->type != DAV_RESOURCE_TYPE_VERSION)
      || (resource->type == DAV_RESOURCE_TYPE_VERSION && resource->baselined))
    return "";

  /* ### what kind of etag to return for collections, activities, etc? */

  if ((serr = svn_fs_node_created_rev(&created_rev, resource->info->root.root,
                                      resource->info->repos_path,
                                      pool)))
    {
      /* ### what to do? */
      svn_error_clear(serr);
      return "";
    }

  /* Use the "weak" format of the etag for collections because our GET
     requests on collections include dynamic data (the HEAD revision,
     the build version of Subversion, etc.). */
  return apr_psprintf(pool, "%s\"%ld/%s\"",
                      resource->collection ? "W/" : "",
                      created_rev,
                      apr_xml_quote_string(pool,
                                           resource->info->repos_path, 1));
}

/* Since dav_svn_getetag() takes a pool argument, this wrapper is for
   the mod_dav hooks vtable entry, which does not. */
static const char * getetag_pathetic(const dav_resource *resource)
{
  return dav_svn_getetag(resource, resource->pool);
}

static dav_error * dav_svn_set_headers(request_rec *r,
                                       const dav_resource *resource)
{
  svn_error_t *serr;
  svn_filesize_t length;
  const char *mimetype = NULL;
  
  if (!resource->exists)
    return NULL;

  /* ### what to do for collections, activities, etc */

  /* make sure the proper mtime is in the request record */
#if 0
  ap_update_mtime(r, resource->info->finfo.mtime);
#endif

  /* ### note that these use r->filename rather than <resource> */
#if 0
  ap_set_last_modified(r);
#endif

  /* generate our etag and place it into the output */
  apr_table_setn(r->headers_out, "ETag",
                 dav_svn_getetag(resource, resource->pool));

  /* we accept byte-ranges */
  apr_table_setn(r->headers_out, "Accept-Ranges", "bytes");

  /* For a directory, we will send text/html or text/xml. If we have a delta
     base, then we will always be generating an svndiff.  Otherwise,
     we need to fetch the appropriate MIME type from the resource's
     properties (and use text/plain if it isn't there). */
  if (resource->collection)
    {
      if (resource->info->repos->xslt_uri)
        mimetype = "text/xml";
      else
        mimetype = "text/html; charset=UTF-8";
    }
  else if (resource->info->delta_base != NULL)
    {
      dav_svn_uri_info info;

      /* First order of business is to parse it. */
      serr = dav_svn_simple_parse_uri(&info, resource,
                                      resource->info->delta_base,
                                      resource->pool);

      /* If we successfully parse the base URL, then send an svndiff. */
      if ((serr == NULL) && (info.rev != SVN_INVALID_REVNUM))
        {
          mimetype = SVN_SVNDIFF_MIME_TYPE;
        }
      svn_error_clear(serr);
    }

  if ((mimetype == NULL)
      && ((resource->type == DAV_RESOURCE_TYPE_VERSION)
          || (resource->type == DAV_RESOURCE_TYPE_REGULAR))
      && (resource->info->repos_path != NULL))
    {
      svn_string_t *value;

      serr = svn_fs_node_prop(&value,
                              resource->info->root.root,
                              resource->info->repos_path,
                              SVN_PROP_MIME_TYPE,
                              resource->pool);
      if (serr != NULL)
        return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                   "could not fetch the resource's MIME type",
                                   resource->pool);

      mimetype = value ? value->data : "text/plain";

      serr = svn_mime_type_validate(mimetype, resource->pool);
      if (serr)
        {
          /* Probably serr->apr == SVN_ERR_BAD_MIME_TYPE, but
             there's no point even checking.  No matter what the
             error is, we can't derive the mime type from the
             svn:mime-type property.  So we resort to the infamous
             "mime type of last resort." */
          svn_error_clear(serr);
          mimetype = "application/octet-stream";
        }

      /* if we aren't sending a diff, then we know the length of the file,
         so set up the Content-Length header */
      serr = svn_fs_file_length(&length,
                                resource->info->root.root,
                                resource->info->repos_path,
                                resource->pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "could not fetch the resource length",
                                     resource->pool);
        }
      ap_set_content_length(r, (apr_off_t) length);
    }

  /* set the discovered MIME type */
  /* ### it would be best to do this during the findct phase... */
  r->content_type = mimetype;

  return NULL;
}

static svn_error_t *dav_svn_write_to_filter(void *baton,
                                            const char *buffer,
                                            apr_size_t *len)
{
  dav_svn_diff_ctx_t *dc = baton;
  apr_bucket_brigade *bb;
  apr_bucket *bkt;
  apr_status_t status;

  /* take the current data and shove it into the filter */
  bb = apr_brigade_create(dc->pool, dc->output->c->bucket_alloc);
  bkt = apr_bucket_transient_create(buffer, *len, dc->output->c->bucket_alloc);
  APR_BRIGADE_INSERT_TAIL(bb, bkt);
  if ((status = ap_pass_brigade(dc->output, bb)) != APR_SUCCESS) {
    return svn_error_create(status, NULL,
                            "Could not write data to filter");
  }

  return SVN_NO_ERROR;
}

static svn_error_t *dav_svn_close_filter(void *baton)
{
  dav_svn_diff_ctx_t *dc = baton;
  apr_bucket_brigade *bb;
  apr_bucket *bkt;
  apr_status_t status;

  /* done with the file. write an EOS bucket now. */
  bb = apr_brigade_create(dc->pool, dc->output->c->bucket_alloc);
  bkt = apr_bucket_eos_create(dc->output->c->bucket_alloc);
  APR_BRIGADE_INSERT_TAIL(bb, bkt);
  if ((status = ap_pass_brigade(dc->output, bb)) != APR_SUCCESS)
    return svn_error_create(status, NULL, "Could not write EOS to filter");

  return SVN_NO_ERROR;
}

static dav_error * dav_svn_deliver(const dav_resource *resource,
                                   ap_filter_t *output)
{
  svn_error_t *serr;
  apr_bucket_brigade *bb;
  apr_bucket *bkt;
  apr_status_t status;

  /* Check resource type */
  if (resource->type != DAV_RESOURCE_TYPE_REGULAR
      && resource->type != DAV_RESOURCE_TYPE_VERSION
      && resource->type != DAV_RESOURCE_TYPE_WORKING) {
    return dav_new_error(resource->pool, HTTP_CONFLICT, 0,
                         "Cannot GET this type of resource.");
  }

  if (resource->collection) {
    const int gen_html = !resource->info->repos->xslt_uri;
    apr_hash_t *entries;
    apr_pool_t *entry_pool;
    apr_array_header_t *sorted;
    int i;

    /* XML schema for the directory index if xslt_uri is set:

       <?xml version="1.0"?>
       <?xml-stylesheet type="text/xsl" href="[info->repos->xslt_uri]"?> */
    static const char xml_index_dtd[] =
      "<!DOCTYPE svn [\n"
      "  <!ELEMENT svn   (index)>\n"
      "  <!ATTLIST svn   version CDATA #REQUIRED\n"
      "                  href    CDATA #REQUIRED>\n"
      "  <!ELEMENT index (updir?, (file | dir)*)>\n"
      "  <!ATTLIST index name    CDATA #IMPLIED\n"
      "                  path    CDATA #IMPLIED\n"
      "                  rev     CDATA #IMPLIED>\n"
      "  <!ELEMENT updir EMPTY>\n"
      "  <!ELEMENT file  (prop)*>\n"
      "  <!ATTLIST file  name    CDATA #REQUIRED\n"
      "                  href    CDATA #REQUIRED>\n"
      "  <!ELEMENT dir   (prop)*>\n"
      "  <!ATTLIST dir   name    CDATA #REQUIRED\n"
      "                  href    CDATA #REQUIRED>\n"
      "  <!ELEMENT prop  (#PCDATA)>\n"
      "  <!ATTLIST prop  name    CDATA #REQUIRED>\n"
      "]>\n";

    /* <svn version="0.13.1 (dev-build)"
            href="http://subversion.tigris.org">
         <index name="[info->repos->repo_name]"
                path="[info->repos_path]"
                rev="[info->root.rev]">
           <file name="foo">
             <prop name="mime-type">image/png</prop>
           </file>
           <dir name="bar"/>
         </index>
       </svn> */

    serr = svn_fs_dir_entries(&entries, resource->info->root.root,
                              resource->info->repos_path, resource->pool);
    if (serr != NULL)
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "could not fetch directory entries",
                                 resource->pool);

    bb = apr_brigade_create(resource->pool, output->c->bucket_alloc);

    if (gen_html)
      {
        const char *title;
        if (resource->info->repos_path == NULL)
          title = "unknown location";
        else
          title = resource->info->repos_path;

        if (SVN_IS_VALID_REVNUM(resource->info->root.rev))
          title = apr_psprintf(resource->pool,
                               "Revision %ld: %s",
                               resource->info->root.rev, title);

        if (resource->info->repos->repo_name)
          title = apr_psprintf(resource->pool, "%s - %s",
                               resource->info->repos->repo_name,
                               title);

        ap_fprintf(output, bb, "<html><head><title>%s</title></head>\n"
                   "<body>\n <h2>%s</h2>\n <ul>\n", title, title);
      }
    else
      {
        const char *name = resource->info->repos->repo_name;
        const char *href = resource->info->repos_path;

        ap_fputs(output, bb, "<?xml version=\"1.0\"?>\n");
        ap_fprintf(output, bb,
                   "<?xml-stylesheet type=\"text/xsl\" href=\"%s\"?>\n",
                   resource->info->repos->xslt_uri);
        ap_fputs(output, bb, xml_index_dtd);
        ap_fputs(output, bb,
                 "<svn version=\"" SVN_VERSION "\"\n"
                 "     href=\"http://subversion.tigris.org/\">\n");
        ap_fputs(output, bb, "  <index");
        if (name)
          ap_fprintf(output, bb, " name=\"%s\"",
                     apr_xml_quote_string(resource->pool, name, 1));
        if (SVN_IS_VALID_REVNUM(resource->info->root.rev))
          ap_fprintf(output, bb, " rev=\"%ld\"",
                     resource->info->root.rev);
        if (href)
          ap_fprintf(output, bb, " path=\"%s\"",
                     apr_xml_quote_string(resource->pool,
                                          href,
                                          1));
        ap_fputs(output, bb, ">\n");
      }

    if (resource->info->repos_path && resource->info->repos_path[1] != '\0')
      {
        if (gen_html)
          ap_fprintf(output, bb, "  <li><a href=\"../\">..</a></li>\n");
        else
          ap_fprintf(output, bb, "    <updir />\n");
      }

    /* get a sorted list of the entries */
    sorted = svn_sort__hash(entries, svn_sort_compare_items_as_paths,
                            resource->pool);

    entry_pool = svn_pool_create(resource->pool);

    for (i = 0; i < sorted->nelts; ++i)
      {
        const svn_sort__item_t *item = &APR_ARRAY_IDX(sorted, i,
                                                      const svn_sort__item_t);
        const svn_fs_dirent_t *entry = item->value;
        const char *name = item->key;
        const char *href = name;
        svn_boolean_t is_dir = (entry->kind == svn_node_dir);

        /* append a trailing slash onto the name for directories. we NEED
           this for the href portion so that the relative reference will
           descend properly. for the visible portion, it is just nice. */
        /* ### The xml output doesn't like to see a trailing slash on
           ### the visible portion, so avoid that. */
        if (is_dir)
          href = apr_pstrcat(entry_pool, href, "/", NULL);

        if (gen_html)
          name = href;

	/* We quote special characters in both XML and HTML. */
	name = apr_xml_quote_string(entry_pool, name, !gen_html);

        href = ap_escape_uri(entry_pool, href);
	href = apr_xml_quote_string(entry_pool, href, 1);

        if (gen_html)
          {
            ap_fprintf(output, bb,
                       "  <li><a href=\"%s\">%s</a></li>\n",
                       href, name);
          }
        else
          {
            const char *const tag = (is_dir ? "dir" : "file");
            
            /* ### This is where the we could search for props */

            ap_fprintf(output, bb,
                       "    <%s name=\"%s\" href=\"%s\"></%s>\n",
                       tag, name, href, tag);
          }
        svn_pool_clear(entry_pool);
      }

    svn_pool_destroy(entry_pool);

    if (gen_html)
      ap_fputs(output, bb,
               " </ul>\n <hr noshade><em>Powered by "
               "<a href=\"http://subversion.tigris.org/\">Subversion</a> "
               "version " SVN_VERSION "."
               "</em>\n</body></html>");
    else
      ap_fputs(output, bb, "  </index>\n</svn>\n");

    bkt = apr_bucket_eos_create(output->c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, bkt);
    if ((status = ap_pass_brigade(output, bb)) != APR_SUCCESS) {
      return dav_new_error(resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                           "Could not write EOS to filter.");
    }

    return NULL;
  }


  /* If we have a base for a delta, then we want to compute an svndiff
     between the provided base and the requested resource. For a simple
     request, then we just grab the file contents. */
  if (resource->info->delta_base != NULL)
    {
      dav_svn_uri_info info;
      svn_fs_root_t *root;
      svn_boolean_t is_file;
      svn_txdelta_stream_t *txd_stream;
      svn_stream_t *o_stream;
      svn_txdelta_window_handler_t handler;
      void * h_baton;
      dav_svn_diff_ctx_t dc = { 0 };

      /* First order of business is to parse it. */
      serr = dav_svn_simple_parse_uri(&info, resource,
                                      resource->info->delta_base,
                                      resource->pool);

      /* If we successfully parse the base URL, then send an svndiff. */
      if ((serr == NULL) && (info.rev != SVN_INVALID_REVNUM))
        {
          /* We are always accessing the base resource by ID, so open
             an ID root. */
          serr = svn_fs_revision_root(&root, resource->info->repos->fs,
                                      info.rev, resource->pool);
          if (serr != NULL)
            return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                       "could not open a root for the base",
                                       resource->pool);

          /* verify that it is a file */
          serr = svn_fs_is_file(&is_file, root, info.repos_path, 
                                resource->pool);
          if (serr != NULL)
            return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                       "could not determine if the base "
                                       "is really a file",
                                       resource->pool);
          if (!is_file)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
                                 "the delta base does not refer to a file");

          /* Okay. Let's open up a delta stream for the client to read. */
          serr = svn_fs_get_file_delta_stream(&txd_stream,
                                              root, info.repos_path,
                                              resource->info->root.root,
                                              resource->info->repos_path,
                                              resource->pool);
          if (serr != NULL)
            return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                       "could not prepare to read a delta",
                                       resource->pool);

          /* create a stream that svndiff data will be written to,
             which will copy it to the network */
          dc.output = output;
          dc.pool = resource->pool;
          o_stream = svn_stream_create(&dc, resource->pool);
          svn_stream_set_write(o_stream, dav_svn_write_to_filter);
          svn_stream_set_close(o_stream, dav_svn_close_filter);

          /* get a handler/baton for writing into the output stream */
          svn_txdelta_to_svndiff(o_stream, resource->pool, &handler, &h_baton);

          /* got everything set up. read in delta windows and shove them into
             the handler, which pushes data into the output stream, which goes
             to the network. */
          serr = svn_txdelta_send_txstream(txd_stream, handler, h_baton,
                                           resource->pool);
          if (serr != NULL)
            return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                       "could not deliver the txdelta stream",
                                       resource->pool);


          return NULL;
        }
      else
        {
          svn_error_clear(serr);
        }
    }

  /* resource->info->delta_base is NULL, or we had an invalid base URL */
    {
      svn_stream_t *stream;
      char *block;

      serr = svn_fs_file_contents(&stream,
                                  resource->info->root.root,
                                  resource->info->repos_path,
                                  resource->pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "could not prepare to read the file",
                                     resource->pool);
        }

      /* ### one day in the future, we can create a custom bucket type
         ### which will read from the FS stream on demand */

      block = apr_palloc(resource->pool, SVN_STREAM_CHUNK_SIZE);
      while (1) {
        apr_size_t bufsize = SVN_STREAM_CHUNK_SIZE;

        /* read from the FS ... */
        serr = svn_stream_read(stream, block, &bufsize);
        if (serr != NULL)
          {
            return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                       "could not read the file contents",
                                       resource->pool);
          }
        if (bufsize == 0)
          break;

        /* build a brigade and write to the filter ... */
        bb = apr_brigade_create(resource->pool, output->c->bucket_alloc);
        bkt = apr_bucket_transient_create(block, bufsize, 
                                          output->c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, bkt);
        if ((status = ap_pass_brigade(output, bb)) != APR_SUCCESS) {
          /* ### what to do with status; and that HTTP code... */
          return dav_new_error(resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                               "Could not write data to filter.");
        }
      }

      /* done with the file. write an EOS bucket now. */
      bb = apr_brigade_create(resource->pool, output->c->bucket_alloc);
      bkt = apr_bucket_eos_create(output->c->bucket_alloc);
      APR_BRIGADE_INSERT_TAIL(bb, bkt);
      if ((status = ap_pass_brigade(output, bb)) != APR_SUCCESS) {
        /* ### what to do with status; and that HTTP code... */
        return dav_new_error(resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                             "Could not write EOS to filter.");
      }

      return NULL;
    }
}

static dav_error * dav_svn_create_collection(dav_resource *resource)
{
  svn_error_t *serr;
  dav_error *err;

  if (resource->type != DAV_RESOURCE_TYPE_WORKING
      && resource->type != DAV_RESOURCE_TYPE_REGULAR)
    {
      return dav_new_error(resource->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                           "Collections can only be created within a working "
                           "or regular collection [at this time].");
    }

  /* ...regular resources allowed only if autoversioning is turned on. */
  if (resource->type == DAV_RESOURCE_TYPE_REGULAR
      && ! (resource->info->repos->autoversioning))
    return dav_new_error(resource->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                         "MKCOL called on regular resource, but "
                         "autoversioning is not active.");

  /* ### note that the parent was checked out at some point, and this
     ### is being preformed relative to the working rsrc for that parent */

  /* Auto-versioning mkcol of regular resource: */
  if (resource->type == DAV_RESOURCE_TYPE_REGULAR)
    {
      /* Change the VCR into a WR, in place.  This creates a txn and
         changes resource->info->root from a rev-root into a txn-root. */
      err = dav_svn_checkout(resource,
                             1 /* auto-checkout */,
                             0, 0, 0, NULL, NULL);
      if (err)
        return err;
    }

  if ((serr = svn_fs_make_dir(resource->info->root.root,
                              resource->info->repos_path,
                              resource->pool)) != NULL)
    {
      /* ### need a better error */
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Could not create the collection.",
                                 resource->pool);
    }

  /* Auto-versioning commit of the txn. */
  if (resource->info->auto_checked_out)
    {
      /* This also changes the WR back into a VCR, in place. */
      err = dav_svn_checkin(resource, 0, NULL);
      if (err)
        return err;
    }

  return NULL;
}

static dav_error * dav_svn_copy_resource(const dav_resource *src,
                                         dav_resource *dst,
                                         int depth,
                                         dav_response **response)
{
  svn_error_t *serr;
  dav_error *err;
  const char *src_repos_path, *dst_repos_path;

  /* ### source must be from a collection under baseline control. the
     ### baseline will (implicitly) indicate the source revision, and the
     ### path will be derived simply from the URL path */

  /* ### the destination's parent must be a working collection */

  /* ### ben goofing around: */
  /*  char *msg;
      apr_psprintf
      (src->pool, "Got a COPY request with src arg '%s' and dst arg '%s'",
      src->uri, dst->uri);
      
      return dav_new_error(src->pool, HTTP_NOT_IMPLEMENTED, 0, msg);
  */

  /* ### Safeguard: see issue #916, whereby we're allowing an
     auto-checkout of a baseline for PROPPATCHing, *without* creating
     a new baseline afterwards.  We need to safeguard here that nobody
     is calling COPY with the baseline as a Destination! */
  if (dst->baselined && dst->type == DAV_RESOURCE_TYPE_VERSION)
    return dav_new_error(src->pool, HTTP_PRECONDITION_FAILED, 0,
                         "Illegal: COPY Destination is a baseline.");

  if (dst->type == DAV_RESOURCE_TYPE_REGULAR
      && !(dst->info->repos->autoversioning))
    return dav_new_error(dst->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                         "COPY called on regular resource, but "
                         "autoversioning is not active.");

  /* Auto-versioning copy of regular resource: */
  if (dst->type == DAV_RESOURCE_TYPE_REGULAR)
    {
      /* Change the VCR into a WR, in place.  This creates a txn and
         changes dst->info->root from a rev-root into a txn-root. */
      err = dav_svn_checkout(dst,
                             1 /* auto-checkout */,
                             0, 0, 0, NULL, NULL);
      if (err)
        return err;
    }

  serr = svn_path_get_absolute(&src_repos_path,
                               svn_repos_path(src->info->repos->repos, 
                                              src->pool),
                               src->pool);
  if (!serr)
    serr = svn_path_get_absolute(&dst_repos_path,
                                 svn_repos_path(dst->info->repos->repos, 
                                                dst->pool),
                                 dst->pool);

  if (!serr)
    {
      if (strcmp(src_repos_path, dst_repos_path) != 0)
        return dav_new_error
          (dst->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
           "Copy source and destination are in different repositories.");
      serr = svn_fs_copy(src->info->root.root,  /* root object of src rev*/
                         src->info->repos_path, /* relative path of src */
                         dst->info->root.root,  /* root object of dst txn*/ 
                         dst->info->repos_path, /* relative path of dst */
                         src->pool);
    }
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Unable to make a filesystem copy.",
                               dst->pool);

  /* Auto-versioning commit of the txn. */
  if (dst->info->auto_checked_out)
    {
      /* This also changes the WR back into a VCR, in place. */
      err = dav_svn_checkin(dst, 0, NULL);
      if (err)
        return err;
    }

  return NULL;
}


static dav_error * dav_svn_remove_resource(dav_resource *resource,
                                           dav_response **response)
{
  svn_error_t *serr;
  dav_error *err;

  /* Only activities, and working or regular resources can be deleted... */
  if (resource->type != DAV_RESOURCE_TYPE_WORKING
      && resource->type != DAV_RESOURCE_TYPE_REGULAR
      && resource->type != DAV_RESOURCE_TYPE_ACTIVITY)
    return dav_new_error(resource->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                           "DELETE called on invalid resource type.");

  /* ...and regular resources only if autoversioning is turned on. */
  if (resource->type == DAV_RESOURCE_TYPE_REGULAR
      && ! (resource->info->repos->autoversioning))
    return dav_new_error(resource->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                         "DELETE called on regular resource, but "
                         "autoversioning is not active.");

  /* Handle activity deletions (early exit). */
  if (resource->type == DAV_RESOURCE_TYPE_ACTIVITY)
    {
      return dav_svn_delete_activity(resource->info->repos,
                                     resource->info->root.activity_id);
    }

  /* ### note that the parent was checked out at some point, and this
     ### is being preformed relative to the working rsrc for that parent */

  /* NOTE: strictly speaking, we cannot determine whether the parent was
     ever checked out, and that this working resource is relative to that
     checked out parent. It is entirely possible the client checked out
     the target resource and just deleted it. Subversion doesn't mind, but
     this does imply we are not enforcing the "checkout the parent, then
     delete from within" semantic. */

  /* Auto-versioning delete of regular resource: */
  if (resource->type == DAV_RESOURCE_TYPE_REGULAR)
    {
      /* Change the VCR into a WR, in place.  This creates a txn and
         changes resource->info->root from a rev-root into a txn-root. */
      err = dav_svn_checkout(resource,
                             1 /* auto-checkout */,
                             0, 0, 0, NULL, NULL);
      if (err)
        return err;
    }

  /* Sanity check: an svn client may have sent a custom request header
     containing the revision of the item it thinks it's deleting.  In
     this case, we enforce the svn-specific semantic that the item
     must be up-to-date. */
  if (SVN_IS_VALID_REVNUM(resource->info->version_name))
    {
      svn_revnum_t created_rev;
      serr = svn_fs_node_created_rev(&created_rev,
                                     resource->info->root.root,
                                     resource->info->repos_path,
                                     resource->pool);
      if (serr)
        return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                   "Could not get created rev of resource",
                                   resource->pool);

      if (resource->info->version_name < created_rev)
        {
          serr = svn_error_createf(SVN_ERR_RA_OUT_OF_DATE, NULL,
                                   "Item '%s' is out of date", 
                                   resource->info->repos_path);
          return dav_svn_convert_err(serr, HTTP_CONFLICT,
                                     "Can't DELETE out-of-date resource",
                                     resource->pool);
        }
    }

  if ((serr = svn_fs_delete(resource->info->root.root,
                            resource->info->repos_path,
                            resource->pool)) != NULL)
    {
      /* ### need a better error */
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "Could not delete the resource",
                                 resource->pool);
    }

  /* Auto-versioning commit of the txn. */
  if (resource->info->auto_checked_out)
    {
      /* This also changes the WR back into a VCR, in place. */
      err = dav_svn_checkin(resource, 0, NULL);
      if (err)
        return err;
    }

  return NULL;
}


static dav_error * dav_svn_move_resource(dav_resource *src,
                                         dav_resource *dst,
                                         dav_response **response)
{
  svn_error_t *serr;
  dav_error *err;

  /* NOTE: The svn client does not call the MOVE method yet. Strictly
     speaking, we do not need to implement this repository function.
     But we do so anyway, so non-deltaV clients can work against the
     repository when autoversioning is turned on.  Like the svn client,
     itself, we define a move to be a copy + delete within a single txn. */

  /* Because we have no 'atomic' move, we only allow this method on
     two regular resources with autoversioning active.  That way we
     can auto-checkout a single resource and do the copy + delete
     within a single txn.  (If we had two working resources, which txn
     would we use?) */
  if (src->type != DAV_RESOURCE_TYPE_REGULAR
      || dst->type != DAV_RESOURCE_TYPE_REGULAR
      || !(src->info->repos->autoversioning))
    return dav_new_error(dst->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                         "MOVE only allowed on two public URIs, and "
                         "autoversioning must be active.");

  /* Change the dst VCR into a WR, in place.  This creates a txn and
     changes dst->info->root from a rev-root into a txn-root. */
  err = dav_svn_checkout(dst,
                         1 /* auto-checkout */,
                         0, 0, 0, NULL, NULL);
  if (err)
    return err;

  /* Copy the src to the dst. */
  serr = svn_fs_copy(src->info->root.root,  /* the root object of src rev*/
                     src->info->repos_path, /* the relative path of src */
                     dst->info->root.root,  /* the root object of dst txn*/ 
                     dst->info->repos_path, /* the relative path of dst */
                     src->pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Unable to make a filesystem copy.",
                               dst->pool);

  /* Notice: we're deleting the src repos path from the dst's txn_root. */
  if ((serr = svn_fs_delete(dst->info->root.root,
                            src->info->repos_path,
                            dst->pool)) != NULL)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Could not delete the src resource.",
                               dst->pool);

  /* Commit:  this also changes the WR back into a VCR, in place. */
  err = dav_svn_checkin(dst, 0, NULL);
  if (err)
    return err;

  return NULL;
}


static dav_error * dav_svn_do_walk(dav_svn_walker_context *ctx, int depth)
{
  const dav_walk_params *params = ctx->params;
  int isdir = ctx->res.collection;
  dav_error *err;
  svn_error_t *serr;
  apr_hash_index_t *hi;
  apr_size_t path_len;
  apr_size_t uri_len;
  apr_size_t repos_len;
  apr_hash_t *children;

  /* The current resource is a collection (possibly here thru recursion)
     and this is the invocation for the collection. Alternatively, this is
     the first [and only] entry to do_walk() for a member resource, so
     this will be the invocation for the member. */
  err = (*params->func)(&ctx->wres,
                        isdir ? DAV_CALLTYPE_COLLECTION : DAV_CALLTYPE_MEMBER);
  if (err != NULL)
    return err;

  /* if we are not to recurse, or this is a member, then we're done */
  if (depth == 0 || !isdir)
    return NULL;

  /* ### for now, let's say that working resources have no children. of
     ### course, this isn't true (or "right") for working collections, but
     ### we don't actually need to do a walk right now. */
  if (params->root->type == DAV_RESOURCE_TYPE_WORKING)
    return NULL;

  /* ### need to allow more walking in the future */
  if (params->root->type != DAV_RESOURCE_TYPE_REGULAR)
    {
      return dav_new_error(params->pool, HTTP_METHOD_NOT_ALLOWED, 0,
                           "Walking the resource hierarchy can only be done "
                           "on 'regular' resources [at this time].");
    }

  /* assert: collection resource. isdir == TRUE. repos_path != NULL. */

  /* append "/" to the paths, in preparation for appending child names.
     don't add "/" if the paths are simply "/" */
  if (ctx->info.uri_path->data[ctx->info.uri_path->len - 1] != '/')
    svn_stringbuf_appendcstr(ctx->info.uri_path, "/");
  if (ctx->repos_path->data[ctx->repos_path->len - 1] != '/')
    svn_stringbuf_appendcstr(ctx->repos_path, "/");

  /* NOTE: the URI should already have a trailing "/" */

  /* fix up the dependent pointers */
  ctx->info.repos_path = ctx->repos_path->data;

  /* all of the children exist. also initialize the collection flag. */
  ctx->res.exists = TRUE;
  ctx->res.collection = FALSE;

  /* remember these values so we can chop back to them after each time
     we append a child name to the path/uri/repos */
  path_len = ctx->info.uri_path->len;
  uri_len = ctx->uri->len;
  repos_len = ctx->repos_path->len;

  /* fetch this collection's children */
  serr = svn_fs_dir_entries(&children, ctx->info.root.root,
                            ctx->info.repos_path, params->pool);
  if (serr != NULL)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "could not fetch collection members",
                               params->pool);

  /* iterate over the children in this collection */
  for (hi = apr_hash_first(params->pool, children); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      svn_fs_dirent_t *dirent;

      /* fetch one of the children */
      apr_hash_this(hi, &key, &klen, &val);
      dirent = val;

      /* authorize access to this resource, if applicable */
      if (params->walk_type & DAV_WALKTYPE_AUTH)
        {
          /* ### how/what to do? */
        }

      /* append this child to our buffers */
      svn_stringbuf_appendbytes(ctx->info.uri_path, key, klen);
      svn_stringbuf_appendbytes(ctx->uri, key, klen);
      svn_stringbuf_appendbytes(ctx->repos_path, key, klen);

      /* reset the pointers since the above may have changed them */
      ctx->res.uri = ctx->uri->data;
      ctx->info.repos_path = ctx->repos_path->data;

      if (dirent->kind == svn_node_file)
        {
          err = (*params->func)(&ctx->wres, DAV_CALLTYPE_MEMBER);
          if (err != NULL)
            return err;
        }
      else
        {
          /* this resource is a collection */
          ctx->res.collection = TRUE;

          /* append a slash to the URI (the path doesn't need it yet) */
          svn_stringbuf_appendcstr(ctx->uri, "/");
          ctx->res.uri = ctx->uri->data;

          /* recurse on this collection */
          err = dav_svn_do_walk(ctx, depth - 1);
          if (err != NULL)
            return err;

          /* restore the data */
          ctx->res.collection = 0;
        }

      /* chop the child off the paths and uri. NOTE: no null-term. */
      ctx->info.uri_path->len = path_len;
      ctx->uri->len = uri_len;
      ctx->repos_path->len = repos_len;
    }

  return NULL;
}

static dav_error * dav_svn_walk(const dav_walk_params *params, int depth,
                                dav_response **response)
{
  dav_svn_walker_context ctx = { 0 };
  dav_error *err;

  ctx.params = params;

  ctx.wres.walk_ctx = params->walk_ctx;
  ctx.wres.pool = params->pool;
  ctx.wres.resource = &ctx.res;

  /* copy the resource over and adjust the "info" reference */
  ctx.res = *params->root;
  ctx.info = *ctx.res.info;

  ctx.res.info = &ctx.info;

  /* operate within the proper pool */
  ctx.res.pool = params->pool;

  /* Don't monkey with the path from params->root. Create a new one.
     This path will then be extended/shortened as necessary. */
  ctx.info.uri_path = svn_stringbuf_dup(ctx.info.uri_path, params->pool);

  /* prep the URI buffer */
  ctx.uri = svn_stringbuf_create(params->root->uri, params->pool);

  /* same for repos_path */
  if (ctx.info.repos_path == NULL)
    ctx.repos_path = NULL;
  else
    ctx.repos_path = svn_stringbuf_create(ctx.info.repos_path, params->pool);

  /* if we have a collection, then ensure the URI has a trailing "/" */
  /* ### get_resource always kills the trailing slash... */
  if (ctx.res.collection && ctx.uri->data[ctx.uri->len - 1] != '/') {
    svn_stringbuf_appendcstr(ctx.uri, "/");
  }

  /* the current resource's URI is stored in the (telescoping) ctx.uri */
  ctx.res.uri = ctx.uri->data;

  /* the current resource's repos_path is stored in ctx.repos_path */
  if (ctx.repos_path != NULL)
    ctx.info.repos_path = ctx.repos_path->data;

  /* Create a pool usable by the response. */
  ctx.info.pool = svn_pool_create(params->pool);

  /* ### is the root already/always open? need to verify */

  /* always return the error, and any/all multistatus responses */
  err = dav_svn_do_walk(&ctx, depth);
  *response = ctx.wres.response;

  return err;
}


/*** Utility functions for resource management ***/

dav_resource *dav_svn_create_working_resource(dav_resource *base,
                                              const char *activity_id,
                                              const char *txn_name,
                                              int tweak_in_place)
{
  const char *path;
  dav_resource *res;

  if (base->baselined)
    path = apr_psprintf(base->pool,
                        "/%s/wbl/%s/%ld",
                        base->info->repos->special_uri,
                        activity_id, base->info->root.rev);
  else
    path = apr_psprintf(base->pool, "/%s/wrk/%s%s",
                        base->info->repos->special_uri,
                        activity_id, base->info->repos_path);
  path = svn_path_uri_encode(path, base->pool);

  if (tweak_in_place)
    res = base;
  else
    {
      res = apr_pcalloc(base->pool, sizeof(*res));
      res->info = apr_pcalloc(base->pool, sizeof(*res->info));
    }

  res->type = DAV_RESOURCE_TYPE_WORKING;
  res->exists = TRUE;      /* ### not necessarily correct */
  res->versioned = TRUE;
  res->working = TRUE;
  res->baselined = base->baselined;
  /* collection = FALSE.   ### not necessarily correct */

  res->uri = apr_pstrcat(base->pool, base->info->repos->root_path,
                         path, NULL);
  res->hooks = &dav_svn_hooks_repos;
  res->pool = base->pool;

  res->info->uri_path = svn_stringbuf_create(path, base->pool);
  res->info->repos = base->info->repos;
  res->info->repos_path = base->info->repos_path;
  res->info->root.rev = base->info->root.rev;
  res->info->root.activity_id = activity_id;
  res->info->root.txn_name = txn_name;

  if (tweak_in_place)
    return NULL;
  else
    return res;
}


dav_error * dav_svn_working_to_regular_resource(dav_resource *resource)
{
  dav_resource_private *priv = resource->info;
  dav_svn_repos *repos = priv->repos;
  const char *path;
  svn_error_t *serr;

  /* no need to change the repos object or repos_path */

  /* set type back to REGULAR */
  resource->type = DAV_RESOURCE_TYPE_REGULAR;

  /* remove the working flag */
  resource->working = FALSE;

  /* Change the URL into either a baseline-collection or a public one. */
  if (priv->root.rev == SVN_INVALID_REVNUM)
    {
      serr = svn_fs_youngest_rev(&priv->root.rev, repos->fs, resource->pool);
      if (serr != NULL)
        return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                   "Could not determine youngest rev.",
                                   resource->pool);
      
      /* create public URL */
      path = apr_psprintf(resource->pool, "%s", priv->repos_path);
    }
  else
    {
      /* if rev was specific, create baseline-collection URL */
      path = dav_svn_build_uri(repos, DAV_SVN_BUILD_URI_BC,
                               priv->root.rev, priv->repos_path,
                               0, resource->pool);
    }
  path = svn_path_uri_encode(path, resource->pool);
  priv->uri_path = svn_stringbuf_create(path, resource->pool);

  /* change root.root back into a revision root. */
  serr = svn_fs_revision_root(&priv->root.root, repos->fs,
                              priv->root.rev, resource->pool);
  if (serr != NULL)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Could not open revision root.",
                               resource->pool);
     
  return NULL;
}


dav_error * dav_svn_create_version_resource(dav_resource **version_res,
                                            const char *uri,
                                            apr_pool_t *pool)
{
  int result;
  dav_error *err;

  dav_resource_combined *comb = apr_pcalloc(pool, sizeof(*comb));

  result = dav_svn_parse_version_uri(comb, uri, NULL, 0);
  if (result != 0)
    return dav_new_error(pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                         "Could not parse version resource uri.");

  err = dav_svn_prep_version(comb);
  if (err)
    return err;
  
  *version_res = &comb->res;
  return NULL;
}



const dav_hooks_repository dav_svn_hooks_repos =
{
  1,                            /* special GET handling */
  dav_svn_get_resource,
  dav_svn_get_parent_resource,
  dav_svn_is_same_resource,
  dav_svn_is_parent_resource,
  dav_svn_open_stream,
  dav_svn_close_stream,
  dav_svn_write_stream,
  dav_svn_seek_stream,
  dav_svn_set_headers,
  dav_svn_deliver,
  dav_svn_create_collection,
  dav_svn_copy_resource,
  dav_svn_move_resource,
  dav_svn_remove_resource,
  dav_svn_walk,
  getetag_pathetic
};
