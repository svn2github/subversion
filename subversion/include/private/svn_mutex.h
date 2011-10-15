/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_mutex.h
 * @brief Strutures and functions for mutual exclusion
 */

#ifndef SVN_MUTEX_H
#define SVN_MUTEX_H

#include <apr_thread_mutex.h>

#include "svn_error.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * This is a simple wrapper around @c apr_thread_mutex_t and will be a
 * valid identifier even if APR does not support threading.
 */
#if APR_HAS_THREADS

/** A mutex for synchronization between threads. It may be NULL, in
 * which case no synchronization will take place. The latter is useful
 * when implementing some functionality with optional synchronization.
 */
typedef apr_thread_mutex_t svn_mutex__t;

#else

/** Dummy definition. The content will never be actually accessed.
 */
typedef void svn_mutex__t;

#endif

/** Initialize the @a *mutex. If @a enable_mutex is TRUE, the mutex will
 * actually be created with a lifetime defined by @a result_pool. Otherwise,
 * the pointer will be set to @c NULL and @ref svn_mutex__lock as well as
 * @ref svn_mutex__unlock will be no-ops.
 * 
 * If threading is not supported by APR, this function is a no-op.
 */
svn_error_t *
svn_mutex__init(svn_mutex__t **mutex,
                svn_boolean_t enable_mutex,
                apr_pool_t *result_pool);

/** Acquire the @a mutex, if that has been enabled in @ref svn_mutex__init.
 * Make sure to call @ref svn_mutex__unlock some time later in the same 
 * thread to release the mutex again. Recursive locking are not supported.
 * 
 * @note You should use @ref SVN_MUTEX__WITH_LOCK instead of explicit lock
 * aquisition and release.
 */
svn_error_t *
svn_mutex__lock(svn_mutex__t *mutex);

/** Release the @a mutex, previously acquired using @ref svn_mutex__lock
 * that has been enabled in @ref svn_mutex__init.
 * 
 * Since this is often used as part of the calling function's exit 
 * sequence, we accept that function's current return code in @a err. 
 * If it is not @ref SVN_NO_ERROR, it will be used as the return value -
 * irrespective of the possible internal failures during unlock. If @a err
 * is @ref SVN_NO_ERROR, internal failures of this function will be 
 * reported in the return value.
 * 
 * @note You should use @ref SVN_MUTEX__WITH_LOCK instead of explicit lock
 * aquisition and release.
 */
svn_error_t *
svn_mutex__unlock(svn_mutex__t *mutex,
                  svn_error_t *err);

/** Aquires the @a mutex, executes the expression @a expr and finally
 * releases the @a mutex. If any of these steps fail, the function using
 * this macro will return an @ref svn_error_t. This macro guarantees that
 * the @a mutex will always be unlocked again if it got locked successfully
 * locked by the first step.
 * 
 * @note Prefer using this macro instead of explicit lock aquisition and
 * release.
 */
#define SVN_MUTEX__WITH_LOCK(mutex, expr) \
do {                                      \
  svn_mutex__t *m = (mutex);              \
  svn_error_t *e = svn_mutex__lock(m);    \
  if (e) return svn_error_trace(e);       \
  e = svn_mutex__unlock(m, (expr));       \
  if (e) return svn_error_trace(e);       \
} while (0);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_MUTEX_H */
