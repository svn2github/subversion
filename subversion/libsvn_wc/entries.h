/*
 * entries.h :  manipulating entries
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */


#ifndef SVN_LIBSVN_WC_ENTRIES_H
#define SVN_LIBSVN_WC_ENTRIES_H

#include <apr_pools.h>

#include "svn_types.h"

#include "wc_db.h"
#include "private/svn_sqlite.h"


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** Get an ENTRY for the given LOCAL_ABSPATH.
 *
 * This API does not require an access baton, just a wc_db handle (DB).
 * The requested entry MUST be present and version-controlled when
 * ALLOW_UNVERSIONED is FALSE; otherwise, SVN_ERR_WC_PATH_NOT_FOUND is
 * returned. When ALLOW_UNVERSIONED is TRUE, and the node is not under
 * version control, *ENTRY will be set to NULL (this is easier for callers
 * to handle, than detecting the error and clearing it).
 *
 * If you know the entry is a FILE or DIR, then specify that in KIND. If you
 * are unsure, then specify 'svn_node_unknown' for KIND. This value will be
 * used to optimize the access to the entry, so it is best to know the kind.
 * If you specify FILE/DIR, and the entry is *something else*, then
 * SVN_ERR_NODE_UNEXPECTED_KIND will be returned.
 *
 * For directory nodes, sometimes the caller may want the "stub" from the
 * parent directory. This is usually to examine the DELETED flag. When
 * this is desired, pass TRUE for NEED_PARENT_STUB. It is illegal to pass
 * TRUE if KIND == FILE.
 *
 * If KIND == UNKNOWN, and you request the parent stub, and the node turns
 * out to NOT be a directory, then SVN_ERR_NODE_UNEXPECTED_KIND is returned.
 *
 * If KIND == UNKNOWN, and you request the actual file/dir data (by setting
 * NEED_PARENT_STUB to FALSE), and the node turns out to be a DIR (as
 * specified by the parent), but the subdirectory is NOT present (obstructed
 * or missing), then SVN_ERR_NODE_UNEXPECTED_KIND is returned.
 *
 * NOTE: if SVN_ERR_NODE_UNEXPECTED_KIND is returned, then the ENTRY *IS*
 * valid and may be examined. For any other error, ENTRY *IS NOT* valid.
 *
 * NOTE: if an access baton is available, then it will be examined for
 * cached entries (and this routine may even cache them for you). It is
 * not required, however, to do any access baton management for this API.
 *
 * ENTRY will be allocated in RESULT_POOL, and all temporary allocations
 * will be performed in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__get_entry(const svn_wc_entry_t **entry,
                  svn_wc__db_t *db,
                  const char *local_abspath,
                  svn_boolean_t allow_unversioned,
                  svn_node_kind_t kind,
                  svn_boolean_t need_parent_stub,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool);

/* Is ENTRY in a 'hidden' state in the sense of the 'show_hidden'
 * switches on svn_wc_entries_read(), svn_wc_walk_entries*(), etc.? */
svn_error_t *
svn_wc__entry_is_hidden(svn_boolean_t *hidden, const svn_wc_entry_t *entry);


/* For internal use by entries.c to read/write old-format working copies. */
svn_error_t *
svn_wc__read_entries_old(apr_hash_t **entries,
                         const char *dir_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);

/* For internal use by upgrade.c to write entries in the wc-ng format.  */
svn_error_t *
svn_wc__write_upgraded_entries(svn_wc__db_t *db,
                               svn_sqlite__db_t *sdb,
                               apr_int64_t repos_id,
                               apr_int64_t wc_id,
                               const char *dir_abspath,
                               const char *root_abspath,
                               apr_hash_t *entries,
                               apr_pool_t *scratch_pool);

/* Parse a file external specification in the NULL terminated STR and
   place the path in PATH_RESULT, the peg revision in PEG_REV_RESULT
   and revision number in REV_RESULT.  STR may be NULL, in which case
   PATH_RESULT will be set to NULL and both PEG_REV_RESULT and
   REV_RESULT set to svn_opt_revision_unspecified.

   The format that is read is the same as a working-copy path with a
   peg revision; see svn_opt_parse_path(). */
svn_error_t *
svn_wc__unserialize_file_external(const char **path_result,
                                  svn_opt_revision_t *peg_rev_result,
                                  svn_opt_revision_t *rev_result,
                                  const char *str,
                                  apr_pool_t *pool);

/* Serialize into STR the file external path, peg revision number and
   the operative revision number into a format that
   unserialize_file_external() can parse.  The format is
     %{peg_rev}:%{rev}:%{path}
   where a rev will either be HEAD or the string revision number.  If
   PATH is NULL then STR will be set to NULL.  This method writes to a
   string instead of a svn_stringbuf_t so that the string can be
   protected by write_str(). */
svn_error_t *
svn_wc__serialize_file_external(const char **str,
                                const char *path,
                                const svn_opt_revision_t *peg_rev,
                                const svn_opt_revision_t *rev,
                                apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_ENTRIES_H */
