/*
 * utf.c:  UTF-8 conversion routines
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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



#include <string.h>
#include <ctype.h>
#include <assert.h>

#include <apr_strings.h>
#include <apr_xlate.h>

#include "svn_string.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_utf.h"


#ifdef SVN_UTF8

#define SVN_UTF_NTOU_XLATE_HANDLE "svn-utf-ntou-xlate-handle"
#define SVN_UTF_UTON_XLATE_HANDLE "svn-utf-uton-xlate-handle"

/* Return the apr_xlate handle for converting native characters to UTF-8.
   Create one if it doesn't exist.                                        */
static svn_error_t *
get_ntou_xlate_handle (apr_xlate_t **ret, apr_pool_t *pool)
{
  void *old_handle = NULL;
  apr_pool_t *parent, *global_pool;
  apr_status_t apr_err;

  /* ### I'm worried about the performance implications of searching
   * up the pool tree every time we call this function.  Leaving it in
   * for now, but if this turns out to be a bottleneck, then we should
   * store the xlation handles in some more quickly accessible place.
   *
   *    -kfogel,  7 July 2002
   */
  /* Find the global pool */
  for (global_pool = pool;
       (parent = apr_pool_get_parent (global_pool));
       global_pool = parent) ;

  /* If we already have a handle, just return it. */
  apr_pool_userdata_get (&old_handle, SVN_UTF_NTOU_XLATE_HANDLE, global_pool);
  if (old_handle != NULL) {
    *ret = old_handle;
    return SVN_NO_ERROR;
  }

  /* Try to create one. */
  apr_err = apr_xlate_open (ret, "UTF-8", APR_LOCALE_CHARSET, global_pool);

  if (apr_err != APR_SUCCESS)
    return svn_error_create (apr_err, 0, NULL, pool,
                             "failed to create a converter to UTF-8");

  /* Save it for later. */
  apr_pool_userdata_set (*ret, SVN_UTF_NTOU_XLATE_HANDLE,
                         apr_pool_cleanup_null, global_pool);

  return SVN_NO_ERROR;
}


/* Return the apr_xlate handle for converting UTF-8 to native characters.
   Create one if it doesn't exist.                                        */
static svn_error_t *
get_uton_xlate_handle (apr_xlate_t **ret, apr_pool_t *pool)
{
  void *old_handle = NULL;
  apr_pool_t *parent, *global_pool;
  apr_status_t apr_err;

  /* ### I'm worried about the performance implications of searching
   * up the pool tree every time we call this function.  Leaving it in
   * for now, but if this turns out to be a bottleneck, then we should
   * store the xlation handles in some more quickly accessible place.
   *
   *    -kfogel,  7 July 2002
   */
  /* Find the global pool */
  for (global_pool = pool;
       (parent = apr_pool_get_parent (global_pool));
       global_pool = parent) ;

  /* If we already have a handle, just return it. */
  apr_pool_userdata_get (&old_handle, SVN_UTF_UTON_XLATE_HANDLE, global_pool);
  if (old_handle != NULL) {
    *ret = old_handle;
    return SVN_NO_ERROR;
  }

  /* Try to create one. */
  apr_err = apr_xlate_open (ret, APR_LOCALE_CHARSET, "UTF-8", global_pool);

  if (apr_err != APR_SUCCESS)
    return svn_error_create (apr_err, 0, NULL, pool,
                             "failed to create a converter from UTF-8");

  /* Save it for later. */
  apr_pool_userdata_set (*ret, SVN_UTF_UTON_XLATE_HANDLE,
                         apr_pool_cleanup_null, global_pool);

  return SVN_NO_ERROR;
}


/* Convert SRC_LENGTH bytes of SRC_DATA in CONVSET, store the result
   in *DEST, which is allocated in POOL. */
static svn_error_t *
convert_to_stringbuf (apr_xlate_t *convset,
                      const char *src_data,
                      apr_size_t src_length,
                      svn_stringbuf_t **dest,
                      apr_pool_t *pool)
{
  apr_size_t buflen = src_length;
  apr_status_t apr_err;
  apr_size_t srclen = src_length;
  apr_size_t destlen = 0;
  char *destbuf;

  /* Initialize *DEST to an empty stringbuf. */
  *dest = svn_stringbuf_create ("", pool);
  destbuf = (*dest)->data;

  do 
    {
      /* A 1:2 ratio of input characters to output characters should
         be enough for most translations, and conveniently enough, if
         it isn't, we'll grow the buffer size by 2 again. */
      if (destlen == 0)
        buflen *= 2;

      /* Ensure that *DEST has sufficient storage for the translated
         result. */
      svn_stringbuf_ensure (*dest, buflen + 1);

      /* Update the destination buffer pointer to the first character
         after already-converted output. */
      destbuf = (*dest)->data + (*dest)->len;

      /* Set up state variables for xlate. */
      destlen = buflen - (*dest)->len;

      /* Attempt the conversion. */
      apr_err = apr_xlate_conv_buffer (convset, 
                                       src_data + (src_length - srclen), 
                                       &srclen,
                                       destbuf, 
                                       &destlen);

      /* Now, updated the *DEST->len to track the amount of output data
         churned out so far from this loop. */
      (*dest)->len += ((buflen - (*dest)->len) - destlen);

    } while ((! apr_err) && srclen);

  /* If we exited the loop with an error, return the error. */
  if (apr_err)
    return svn_error_create (apr_err, 0, NULL, pool,
                             "failure during string recoding");
  
  /* Else, exited do to success.  Trim the result buffer down to the
     right length. */
  (*dest)->data[(*dest)->len] = '\0';

  return SVN_NO_ERROR;
}

#else /* ! SVN_UTF8 */

/* Return an SVN_ERR_UNSUPPORTED_FEATURE error allocated from POOL if
   the first LEN bytes of DATA contain any characters with the eighth
   bit set, or any escape (ascii 27) characters.  Otherwise, return
   SVN_NO_ERROR.  */
static svn_error_t *
check_non_ascii (const char *data, apr_size_t len, apr_pool_t *pool)
{
  for (; len > 0; --len, data++)
    {
      if (/* Check if eighth bit set: */
          ((*(unsigned char *)data) & 128)
          /* Look for ESC, to detect ISO-2022 etc: */
          || *(unsigned char *)data == 27)
        return svn_error_create (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool,
                                 "non-ascii characters detected, "
                                 "please recompile with --enable-utf8");
    }

  return SVN_NO_ERROR;
}

#endif /* SVN_UTF8 */


svn_error_t *
svn_utf_stringbuf_to_utf8 (svn_stringbuf_t **dest,
                           const svn_stringbuf_t *src,
                           apr_pool_t *pool)
{
#ifdef SVN_UTF8

  /* Get a converter from the native character encoding to UTF-8 */
  apr_xlate_t *convset;
  SVN_ERR (get_ntou_xlate_handle (&convset, pool));

  return convert_to_stringbuf (convset, src->data, src->len, dest, pool);

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src->data, src->len, pool));
  *dest = svn_stringbuf_dup (src, pool);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_string_to_utf8 (const svn_string_t **dest,
                        const svn_string_t *src,
                        apr_pool_t *pool)
{
#ifdef SVN_UTF8

  svn_stringbuf_t *destbuf;
  apr_xlate_t *convset;
  /* Get a converter from the native character encoding to UTF-8 */
  SVN_ERR (get_ntou_xlate_handle (&convset, pool));
  SVN_ERR (convert_to_stringbuf (convset, src->data, src->len, 
                                 &destbuf, pool));
  *dest = svn_string_create_from_buf (destbuf, pool);
  return SVN_NO_ERROR;

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src->data, src->len, pool));
  *dest = svn_string_dup (src, pool);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_cstring_to_utf8_stringbuf (svn_stringbuf_t **dest,
                                   const char *src,
                                   apr_xlate_t *xlator,
                                   apr_pool_t *pool)
{
#ifdef SVN_UTF8

  /* Get a converter from the native character encoding to UTF-8 */
  apr_xlate_t *convset;
  if (! xlator)
    SVN_ERR (get_ntou_xlate_handle (&convset, pool));
  else
    convset = xlator;

  return convert_to_stringbuf (convset, src, strlen (src), dest, pool);

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src, strlen (src), pool));
  *dest = svn_stringbuf_create (src, pool);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_cstring_to_utf8 (const char **dest,
                         const char *src,
                         apr_xlate_t *xlator,
                         apr_pool_t *pool)
{
#ifdef SVN_UTF8

  svn_stringbuf_t *destbuf;
  SVN_ERR (svn_utf_cstring_to_utf8_stringbuf (src, &destbuf, xlator, pool));
  *dest = destbuf->data;
  return SVN_NO_ERROR;

#else /* ! SVN_UTF8 */

  apr_size_t len = strlen (src);
  SVN_ERR (check_non_ascii (src, len, pool));
  *dest = apr_pstrmemdup (pool, src, len);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_stringbuf_from_utf8 (svn_stringbuf_t **dest,
			     const svn_stringbuf_t *src,
			     apr_pool_t *pool)
{
#ifdef SVN_UTF8

  /* Get a converter from UTF-8 to the native character encoding */
  apr_xlate_t *convset;
  SVN_ERR (get_uton_xlate_handle (&convset, pool));

  return convert_to_stringbuf (convset, src->data, src->len, dest, pool);

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src->data, src->len, pool));
  *dest = svn_stringbuf_dup (src, pool);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_string_from_utf8 (const svn_string_t **dest,
                          const svn_string_t *src,
                          apr_pool_t *pool)
{
#ifdef SVN_UTF8

  svn_stringbuf_t *dbuf;

  /* Get a converter from UTF-8 to the native character encoding */
  apr_xlate_t *convset;
  SVN_ERR (get_uton_xlate_handle (&convset, pool));
  SVN_ERR (convert_to_stringbuf (convset, src->data, src->len, &dbuf, pool));
  *dest = svn_string_create_from_buf (dbuf, pool);
  return SVN_NO_ERROR;

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src->data, src->len, pool));
  *dest = svn_string_dup (src, pool);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_cstring_from_utf8 (const char **dest,
                           const char *src,
                           apr_pool_t *pool)
{
#ifdef SVN_UTF8

  svn_stringbuf_t *destbuf;

  /* Get a converter from UTF-8 to the native character encoding */
  apr_xlate_t *convset;
  SVN_ERR (get_uton_xlate_handle (&convset, pool));
  SVN_ERR (convert_to_stringbuf (convset, src, strlen (src), &destbuf, pool));
  *dest = destbuf->data;
  return SVN_NO_ERROR;

#else /* ! SVN_UTF8 */

  apr_size_t len = strlen (src);
  SVN_ERR (check_non_ascii (src, len, pool));
  *dest = apr_pstrmemdup (pool, src, len);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_cstring_from_utf8_stringbuf (const char **dest,
                                     const svn_stringbuf_t *src,
                                     apr_pool_t *pool)
{
#ifdef SVN_UTF8

  svn_stringbuf_t *destbuf;
  SVN_ERR (svn_utf_stringbuf_from_utf8 (src, &destbuf, pool));
  *dest = destbuf->data;
  return SVN_NO_ERROR;

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src->data, src->len, pool));
  *dest = apr_pstrmemdup (pool, src->data, src->len);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


svn_error_t *
svn_utf_cstring_from_utf8_string (const char **dest,
                                  const svn_string_t *src,
                                  apr_pool_t *pool)
{
#ifdef SVN_UTF8

  svn_stringbuf_t *dbuf;

  /* Get a converter from UTF-8 to the native character encoding */
  apr_xlate_t *convset;
  SVN_ERR (get_uton_xlate_handle (&convset, pool));
  SVN_ERR (convert_to_stringbuf (convset, src->data, src->len, &dbuf, pool));
  *dest = dbuf->data;
  return SVN_NO_ERROR;

#else /* ! SVN_UTF8 */

  SVN_ERR (check_non_ascii (src->data, src->len, pool));
  *dest = apr_pstrmemdup (pool, src->data, src->len);
  return SVN_NO_ERROR;

#endif /* SVN_UTF8 */
}


const char *
svn_utf_utf8_to_native (const char *utf8_string,
			char *buf,
                        apr_size_t bufsize)
{
#ifdef SVN_UTF8

  /* Set up state variables for xlate */
  apr_size_t srclen = strlen (utf8_string);
  apr_size_t destlen = bufsize-1;

  /* Ick.  Need a pool here so that we can call apr_xlate_open. */
  apr_pool_t *pool = svn_pool_create (NULL);

  /* Get a converter from UTF-8 to the native character encoding */
  apr_xlate_t *convset;
  if (get_uton_xlate_handle (&convset, pool) != SVN_NO_ERROR) {
    svn_pool_destroy (pool);
    return "(charset translator procurement failed)";
  }

  /* Attempt the conversion */
  if (apr_xlate_conv_buffer(convset, utf8_string, &srclen, buf, &destlen) ==
      APR_SUCCESS)
  {
    /* Conversion succeeded.  Zero-terminate and return buffer */
    buf[bufsize-1-destlen] = '\0';
    svn_pool_destroy (pool);
    return buf;
  }

  svn_pool_destroy (pool);
  return "(charset conversion failed)";

#else /* ! SVN_UTF8 */

  int i;

  /* Just replace non-ASCII characters with '?' here... */

  for (i=0; i<bufsize && *utf8_string; utf8_string++)
    if (*(unsigned char *)utf8_string < 128)
      /* ASCII character */
      buf[i++] = *utf8_string;
    else if(*(unsigned char *)utf8_string >= 192)
      /* First octet of a multibyte sequence */
      buf[i++] = '?';

  buf[i>=bufsize? bufsize-1 : i] = '\0';
  return buf;  

#endif /* SVN_UTF8 */
}


/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
