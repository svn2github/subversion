/*
 * filesize.c -- Utilities for displaying file sizes
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


/*** Includes. ***/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <apr_strings.h>

#include "cl.h"


/*** Code. ***/

const char *
svn_cl__get_base2_unit_file_size(svn_filesize_t size,
                                 svn_boolean_t long_units,
                                 apr_pool_t *result_pool)
{
  static const struct
  {
    svn_filesize_t mask;
    const char *suffix;
    const char *short_suffix;
  }
  order[] =
    {
      {APR_INT64_C(0x0000000000000000), " B",   "B"}, /* byte */
      {APR_INT64_C(0x00000000000003FF), " KiB", "K"}, /* kibi */
      {APR_INT64_C(0x00000000000FFFFF), " MiB", "M"}, /* mibi */
      {APR_INT64_C(0x000000003FFFFFFF), " GiB", "G"}, /* gibi */
      {APR_INT64_C(0x000000FFFFFFFFFF), " TiB", "T"}, /* tibi */
      {APR_INT64_C(0x0003FFFFFFFFFFFF), " EiB", "E"}, /* exbi */
      {APR_INT64_C(0x0FFFFFFFFFFFFFFF), " PiB", "P"}  /* pibi */
    };
  static const apr_size_t order_size = sizeof(order) / sizeof(order[0]);

  const svn_filesize_t abs_size = ((size < 0) ? -size : size);
  double human_readable_size;

  /* Find the size mask for the (absolute) file size. It would be sexy to
     do a binary search here, but with only 7 elements in the array ... */
  apr_size_t index = order_size;
  while (index > 0)
    {
      --index;
      if (abs_size > order[index].mask)
        break;
    }

  /* Adjust the size to the given order of magnitude.

     This is division by (order[index].mask + 1), which is the base-2^10
     magnitude of the size; and that is the same as an arithmetic right
     shift by (index * 10) bits. But we split it into an integer and a
     floating-point division, so that we don't overflow the mantissa at
     very large file sizes. */
  ;
  if ((abs_size >> 10 * index) > 999)
    {
      /* This assertion should never fail, because we only have 4 binary
         digits in the petabyte range and so the number of petabytes can't
         be large enough to enter this conditional block. */
      assert(index < order_size - 1);
      ++index;
    }
  human_readable_size = (index == 0 ? (double)size
                         : (size >> 3 * index) / 128.0 / index);

  /* NOTE: We want to display a locale-specific decimal sepratator, but
           APR's formatter completely ignores the locale. So we use the
           good, old, standard, *dangerous* sprintf() to format the size.

           But, on the brigt side, we've just made sure that the number has
           no more than 3 non-fractional digits. So the call to sprintf()
           here should be safe. */
  {
    const char *const suffix = (long_units ? order[index].suffix
                                : order[index].short_suffix);

    /*   3 digits (or 2 digits and 1 decimal separator)
       + 1 negative sign (which should not appear under normal circumstances)
       + 1 nul terminator
       ---
       = 5 characters of space needed in the buffer. */
    char buffer[8];

    /* When the adjusted size has only one significant digit left of the
       decimal point, show tenths of a unit, too. */
    sprintf(buffer, "%.*f",
            fabs(human_readable_size) < 10.0 ? 1 : 0,
            human_readable_size);
    return apr_pstrcat(result_pool, buffer, suffix, SVN_VA_NULL);
  }
}
