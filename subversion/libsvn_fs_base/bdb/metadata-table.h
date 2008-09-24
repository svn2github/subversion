/* metadata-table.h : internal interface to ops on `metadata' table
 *
 * ====================================================================
 * Copyright (c) 2008 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_LOCKS_TABLE_H
#define SVN_LIBSVN_FS_LOCKS_TABLE_H

#include "svn_fs.h"
#include "svn_error.h"
#include "../trail.h"
#include "../fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Open a `metadata' table in ENV.  If CREATE is non-zero, create
   one if it doesn't exist.  Set *METADATA_P to the new table.
   Return a Berkeley DB error code.  */
int svn_fs_bdb__open_metadata_table(DB **metadata_p,
                                   DB_ENV *env,
                                   svn_boolean_t create);


/* Add metadata to the `metadata' table in FS, as part of TRAIL.

   KEY and VAL should be NULL-terminated strings.  If VAL is NULL,
   the key is removed from the table. */
svn_error_t *svn_fs_bdb__metadata_set(svn_fs_t *fs,
                                      const char *key,
                                      const char *val,
                                      trail_t *trail,
                                      apr_pool_t *pool);


/* Retrieve metadata cooresponding to KEY in FS and put it into *VAL
   allocated in POOL, all as part of TRAIL.  */
svn_error_t *svn_fs_bdb__metadata_get(const char **val,
                                      svn_fs_t *fs,
                                      const char *key,
                                      trail_t *trail,
                                      apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_LOCKS_TABLE_H */
