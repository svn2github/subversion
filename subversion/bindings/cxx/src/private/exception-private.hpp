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
 */

#ifndef __cplusplus
#error "This is a C++ header file."
#endif

#ifndef SVNXX_PRIVATE_EXCEPTION_HPP
#define SVNXX_PRIVATE_EXCEPTION_HPP

#include "svn_error.h"

namespace apache {
namespace subversion {
namespace svnxx {
namespace detail {

/**
 * Given a @a err, if it is not @c nullptr, convert it to a and throw an
 * svn::error or svn::canceled exception; otherwise do nothing.
 */
void checked_call(svn_error_t* const err);

} // namespace detail
} // namespace svnxx
} // namespace subversion
} // namespace apache

#endif // SVNXX_PRIVATE_EXCEPTION_HPP
