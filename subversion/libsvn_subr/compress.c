/*
 * compress.c:  various data compression routines
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


#include <string.h>
#include <assert.h>
#include <zlib.h>
#include "lz4/lz4.h"

#include "private/svn_subr_private.h"
#include "private/svn_error_private.h"

#include "svn_private_config.h"

const char *
svn_zlib__compiled_version(void)
{
  static const char zlib_version_str[] = ZLIB_VERSION;

  return zlib_version_str;
}

const char *
svn_zlib__runtime_version(void)
{
  return zlibVersion();
}


/* The zlib compressBound function was not exported until 1.2.0. */
#if ZLIB_VERNUM >= 0x1200
#define svnCompressBound(LEN) compressBound(LEN)
#else
#define svnCompressBound(LEN) ((LEN) + ((LEN) >> 12) + ((LEN) >> 14) + 11)
#endif

/* For svndiff1, address/instruction/new data under this size will not
   be compressed using zlib as a secondary compressor.  */
#define MIN_COMPRESS_SIZE 512

unsigned char *
svn__encode_uint(unsigned char *p, apr_uint64_t val)
{
  int n;
  apr_uint64_t v;

  /* Figure out how many bytes we'll need.  */
  v = val >> 7;
  n = 1;
  while (v > 0)
    {
      v = v >> 7;
      n++;
    }

  /* Encode the remaining bytes; n is always the number of bytes
     coming after the one we're encoding.  */
  while (--n >= 1)
    *p++ = (unsigned char)(((val >> (n * 7)) | 0x80) & 0xff);

  *p++ = (unsigned char)(val & 0x7f);

  return p;
}

unsigned char *
svn__encode_int(unsigned char *p, apr_int64_t val)
{
  apr_uint64_t value = val;
  value = value & APR_UINT64_C(0x8000000000000000)
        ? APR_UINT64_MAX - (2 * value)
        : 2 * value;

  return svn__encode_uint(p, value);
}

const unsigned char *
svn__decode_uint(apr_uint64_t *val,
                 const unsigned char *p,
                 const unsigned char *end)
{
  apr_uint64_t temp = 0;

  if (end - p > SVN__MAX_ENCODED_UINT_LEN)
    end = p + SVN__MAX_ENCODED_UINT_LEN;

  /* Decode bytes until we're done. */
  while (SVN__PREDICT_TRUE(p < end))
    {
      unsigned int c = *p++;

      if (c < 0x80)
        {
          *val = (temp << 7) | c;
          return p;
        }
      else
        {
          temp = (temp << 7) | (c & 0x7f);
        }
    }

  return NULL;
}

const unsigned char *
svn__decode_int(apr_int64_t *val,
                const unsigned char *p,
                const unsigned char *end)
{
  apr_uint64_t value;
  const unsigned char *result = svn__decode_uint(&value, p, end);

  value = value & 1
        ? (APR_UINT64_MAX - value / 2)
        : value / 2;
  *val = (apr_int64_t)value;

  return result;
}

/* If IN is a string that is >= MIN_COMPRESS_SIZE and the COMPRESSION_LEVEL
   is not SVN_DELTA_COMPRESSION_LEVEL_NONE, zlib compress it and places the
   result in OUT, with an integer prepended specifying the original size.
   If IN is < MIN_COMPRESS_SIZE, or if the compressed version of IN was no
   smaller than the original IN, OUT will be a copy of IN with the size
   prepended as an integer. */
static svn_error_t *
zlib_encode(const char *data,
            apr_size_t len,
            svn_stringbuf_t *out,
            int compression_level)
{
  unsigned long endlen;
  apr_size_t intlen;
  unsigned char buf[SVN__MAX_ENCODED_UINT_LEN], *p;

  svn_stringbuf_setempty(out);
  p = svn__encode_uint(buf, (apr_uint64_t)len);
  svn_stringbuf_appendbytes(out, (const char *)buf, p - buf);

  intlen = out->len;

  /* Compression initialization overhead is considered to large for
     short buffers.  Also, if we don't actually want to compress data,
     ZLIB will produce an output no shorter than the input.  Hence,
     the DATA would directly appended to OUT, so we can do that directly
     without calling ZLIB before. */
  if (len < MIN_COMPRESS_SIZE || compression_level == SVN__COMPRESSION_NONE)
    {
      svn_stringbuf_appendbytes(out, data, len);
    }
  else
    {
      int zerr;

      svn_stringbuf_ensure(out, svnCompressBound(len) + intlen);
      endlen = out->blocksize;

      zerr = compress2((unsigned char *)out->data + intlen, &endlen,
                       (const unsigned char *)data, len,
                       compression_level);
      if (zerr != Z_OK)
        return svn_error_trace(svn_error__wrap_zlib(
                                 zerr, "compress2",
                                 _("Compression of svndiff data failed")));

      /* Compression didn't help :(, just append the original text */
      if (endlen >= len)
        {
          svn_stringbuf_appendbytes(out, data, len);
          return SVN_NO_ERROR;
        }
      out->len = endlen + intlen;
      out->data[out->len] = 0;
    }
  return SVN_NO_ERROR;
}

/* Decode the possibly-zlib compressed string of length INLEN that is in
   IN, into OUT.  We expect an integer is prepended to IN that specifies
   the original size, and that if encoded size == original size, that the
   remaining data is not compressed.
   In that case, we will simply return pointer into IN as data pointer for
   OUT, COPYLESS_ALLOWED has been set.  The, the caller is expected not to
   modify the contents of OUT.
   An error is returned if the decoded length exceeds the given LIMIT.
 */
static svn_error_t *
zlib_decode(const unsigned char *in, apr_size_t inLen, svn_stringbuf_t *out,
            apr_size_t limit)
{
  apr_size_t len;
  apr_uint64_t size;
  const unsigned char *oldplace = in;

  /* First thing in the string is the original length.  */
  in = svn__decode_uint(&size, in, in + inLen);
  len = (apr_size_t)size;
  if (in == NULL || len != size)
    return svn_error_create(SVN_ERR_SVNDIFF_INVALID_COMPRESSED_DATA, NULL,
                            _("Decompression of zlib compressed data failed: no size"));
  if (len > limit)
    return svn_error_create(SVN_ERR_SVNDIFF_INVALID_COMPRESSED_DATA, NULL,
                            _("Decompression of zlib compressed data failed: "
                              "size too large"));

  /* We need to subtract the size of the encoded original length off the
   *      still remaining input length.  */
  inLen -= (in - oldplace);
  if (inLen == len)
    {
      svn_stringbuf_ensure(out, len);
      memcpy(out->data, in, len);
      out->data[len] = 0;
      out->len = len;

      return SVN_NO_ERROR;
    }
  else
    {
      unsigned long zlen = len;
      int zerr;

      svn_stringbuf_ensure(out, len);
      zerr = uncompress((unsigned char *)out->data, &zlen, in, inLen);
      if (zerr != Z_OK)
        return svn_error_trace(svn_error__wrap_zlib(
                                 zerr, "uncompress",
                                 _("Decompression of svndiff data failed")));

      /* Zlib should not produce something that has a different size than the
         original length we stored. */
      if (zlen != len)
        return svn_error_create(SVN_ERR_SVNDIFF_INVALID_COMPRESSED_DATA,
                                NULL,
                                _("Size of uncompressed data "
                                  "does not match stored original length"));
      out->data[zlen] = 0;
      out->len = zlen;
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn__compress_zlib(const void *data, apr_size_t len,
                   svn_stringbuf_t *out,
                   int compression_method)
{
  if (   compression_method < SVN__COMPRESSION_NONE
      || compression_method > SVN__COMPRESSION_ZLIB_MAX)
    return svn_error_createf(SVN_ERR_BAD_COMPRESSION_METHOD, NULL,
                             _("Unsupported compression method %d"),
                             compression_method);

  return zlib_encode(data, len, out, compression_method);
}

svn_error_t *
svn__decompress_zlib(const void *data, apr_size_t len,
                     svn_stringbuf_t *out,
                     apr_size_t limit)
{
  return zlib_decode(data, len, out, limit);
}

svn_error_t *
svn__compress_lz4(const void *data, apr_size_t len,
                  svn_stringbuf_t *out)
{
  apr_size_t hdrlen;
  unsigned char buf[SVN__MAX_ENCODED_UINT_LEN];
  unsigned char *p;
  int compressed_data_len;
  int max_compressed_data_len;

  assert(len <= LZ4_MAX_INPUT_SIZE);

  p = svn__encode_uint(buf, (apr_uint64_t)len);
  hdrlen = p - buf;
  max_compressed_data_len = LZ4_compressBound((int)len);
  svn_stringbuf_setempty(out);
  svn_stringbuf_ensure(out, max_compressed_data_len + hdrlen);
  svn_stringbuf_appendbytes(out, (const char *)buf, hdrlen);
  compressed_data_len = LZ4_compress_default(data, out->data + out->len,
                                             (int)len, max_compressed_data_len);
  if (!compressed_data_len)
    return svn_error_create(SVN_ERR_LZ4_COMPRESSION_FAILED, NULL, NULL);

  if (compressed_data_len >= len)
    {
      /* Compression didn't help :(, just append the original text */
      svn_stringbuf_appendbytes(out, data, len);
    }
  else
    {
      out->len += compressed_data_len;
      out->data[out->len] = 0;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn__decompress_lz4(const void *data, apr_size_t len,
                    svn_stringbuf_t *out,
                    apr_size_t limit)
{
  apr_size_t hdrlen;
  int compressed_data_len;
  int decompressed_data_len;
  apr_uint64_t u64;
  const unsigned char *p = data;
  int rv;

  assert(len <= INT_MAX);
  assert(limit <= INT_MAX);

  /* First thing in the string is the original length.  */
  p = svn__decode_uint(&u64, p, p + len);
  if (p == NULL)
    return svn_error_create(SVN_ERR_SVNDIFF_INVALID_COMPRESSED_DATA, NULL,
                            _("Decompression of compressed data failed: "
                              "no size"));
  if (u64 > limit)
    return svn_error_create(SVN_ERR_SVNDIFF_INVALID_COMPRESSED_DATA, NULL,
                            _("Decompression of compressed data failed: "
                              "size too large"));
  decompressed_data_len = (int)u64;
  hdrlen = p - (const unsigned char *)data;
  compressed_data_len = (int)(len - hdrlen);

  svn_stringbuf_setempty(out);
  svn_stringbuf_ensure(out, decompressed_data_len);

  if (compressed_data_len == decompressed_data_len)
    {
      /* Data is in the original, uncompressed form. */
      memcpy(out->data, p, decompressed_data_len);
    }
  else
    {
      rv = LZ4_decompress_safe((const char *)p, out->data, compressed_data_len,
                               decompressed_data_len);
      if (rv < 0)
        return svn_error_create(SVN_ERR_LZ4_DECOMPRESSION_FAILED, NULL, NULL);

      if (rv != decompressed_data_len)
        return svn_error_create(SVN_ERR_SVNDIFF_INVALID_COMPRESSED_DATA,
                                NULL,
                                _("Size of uncompressed data "
                                  "does not match stored original length"));
    }

  out->data[decompressed_data_len] = 0;
  out->len = decompressed_data_len;

  return SVN_NO_ERROR;
}
