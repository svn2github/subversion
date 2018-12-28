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

#ifndef SVNXX_PRIVATE_DEPTH_HPP
#define SVNXX_PRIVATE_DEPTH_HPP

#include "svnxx/depth.hpp"

#include "svn_types.h"

namespace apache {
namespace subversion {
namespace svnxx {
namespace impl {

/**
 * Convert @a d to an svn_depth_t.
 */
svn_depth_t convert(depth d);

/**
 * Convert @a d to an svn::depth.
 */
depth convert(svn_depth_t d);

} // namespace impl
} // namespace svnxx
} // namespace subversion
} // namespace apache

#endif // SVNXX_PRIVATE_DEPTH_HPP
