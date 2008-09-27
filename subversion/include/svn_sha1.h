/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_sha1.h
 * @brief Converting and comparing SHA1 checksums.
 */

#ifndef SVN_SHA1_H
#define SVN_SHA1_H

#include <apr_pools.h>
#include <apr_sha1.h>
#include "svn_error.h"
#include "svn_pools.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/** The SHA1 digest for the empty string.
 *
 * @since New in 1.6.
 */
SVN_DEPRECATED
const unsigned char *
svn_sha1_empty_string_digest(void);


/** Return the hex representation of @a digest, which must be
 * @c APR_SHA1_DIGESTSIZE bytes long, allocating the string in @a pool.
 *
 * @since New in 1.6.
 */
SVN_DEPRECATED
const char *
svn_sha1_digest_to_cstring_display(const unsigned char digest[],
                                   apr_pool_t *pool);


/** Return the hex representation of @a digest, which must be
 * @c APR_SHA1_DIGESTSIZE bytes long, allocating the string in @a pool.
 * If @a digest is all zeros, then return NULL.
 *
 * @since New in 1.6.
 */
SVN_DEPRECATED
const char *
svn_sha1_digest_to_cstring(const unsigned char digest[],
                           apr_pool_t *pool);


/** Compare digests @a d1 and @a d2, each @c APR_SHA1_DIGESTSIZE bytes long.
 * If neither is all zeros, and they do not match, then return FALSE;
 * else return TRUE.
 *
 * @since New in 1.6.
 */
SVN_DEPRECATED
svn_boolean_t
svn_sha1_digests_match(const unsigned char d1[],
                       const unsigned char d2[]);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_SHA1_H */
