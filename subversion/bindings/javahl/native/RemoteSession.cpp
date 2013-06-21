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
 * @file RemoteSession.cpp
 * @brief Implementation of the class RemoteSession
 */

#include <cstring>
#include <set>

#include "JNIByteArray.h"
#include "JNIStringHolder.h"
#include "JNIUtil.h"

#include "svn_ra.h"
#include "svn_string.h"

#include "CreateJ.h"
#include "EnumMapper.h"
#include "Prompter.h"
#include "Revision.h"
#include "RemoteSession.h"

#include "svn_private_config.h"

#define JAVA_CLASS_REMOTE_SESSION JAVA_PACKAGE "/remote/RemoteSession"

RemoteSession *
RemoteSession::getCppObject(jobject jthis)
{
  static jfieldID fid = 0;
  jlong cppAddr = SVNBase::findCppAddrForJObject(jthis, &fid,
      JAVA_CLASS_REMOTE_SESSION);
  return (cppAddr == 0 ? NULL : reinterpret_cast<RemoteSession *>(cppAddr));
}

jobject
RemoteSession::open(jint jretryAttempts,
                    jstring jurl, jstring juuid,
                    jstring jconfigDirectory,
                    jstring jusername, jstring jpassword,
                    jobject jprompter, jobject jprogress)
{
  JNIEnv *env = JNIUtil::getEnv();

  JNIStringHolder url(jurl);
  if (JNIUtil::isExceptionThrown())
    return NULL;
  env->DeleteLocalRef(jurl);

  JNIStringHolder uuid(juuid);
  if (JNIUtil::isExceptionThrown())
    return NULL;
  env->DeleteLocalRef(juuid);

  JNIStringHolder configDirectory(jconfigDirectory);
  if (JNIUtil::isExceptionThrown())
    return NULL;
  env->DeleteLocalRef(jconfigDirectory);

  JNIStringHolder usernameStr(jusername);
  if (JNIUtil::isExceptionThrown())
    return NULL;
  env->DeleteLocalRef(jusername);

  JNIStringHolder passwordStr(jpassword);
  if (JNIUtil::isExceptionThrown())
    return NULL;
  env->DeleteLocalRef(jpassword);

  Prompter *prompter = NULL;
  if (jprompter != NULL)
    {
      prompter = Prompter::makeCPrompter(jprompter);
      if (JNIUtil::isExceptionThrown())
        return NULL;
    }

  jobject jremoteSession = open(
      jretryAttempts, url, uuid, configDirectory,
      usernameStr, passwordStr, prompter, jprogress);
  if (JNIUtil::isExceptionThrown() || !jremoteSession)
    {
      delete prompter;
      jremoteSession = NULL;
    }
  return jremoteSession;
}

jobject
RemoteSession::open(jint jretryAttempts,
                    const char* url, const char* uuid,
                    const char* configDirectory,
                    const char*  usernameStr, const char*  passwordStr,
                    Prompter* prompter, jobject jprogress)
{
  /*
   * Initialize ra layer if we have not done so yet
   */
  static bool initialized = false;
  if (!initialized)
    {
      SVN_JNI_ERR(svn_ra_initialize(JNIUtil::getPool()), NULL);
      initialized = true;
    }

  jobject jthis_out = NULL;
  RemoteSession* session = new RemoteSession(
      &jthis_out, jretryAttempts, url, uuid, configDirectory,
      usernameStr, passwordStr, prompter, jprogress);
  if (JNIUtil::isJavaExceptionThrown() || !session)
    {
      delete session;
      jthis_out = NULL;
    }
  return jthis_out;
}


namespace{
  struct compare_c_strings
  {
    bool operator()(const char* a, const char* b)
      {
        return (0 < std::strcmp(a, b));
      }
  };
  typedef std::set<const char*, compare_c_strings> attempt_set;
  typedef std::pair<attempt_set::iterator, bool> attempt_insert;
} // anonymous namespace

RemoteSession::RemoteSession(jobject* jthis_out, int retryAttempts,
                             const char* url, const char* uuid,
                             const char* configDirectory,
                             const char*  username, const char*  password,
                             Prompter* prompter, jobject jprogress)
  : m_session(NULL), m_context(NULL)
{
  // Create java session object
  JNIEnv *env = JNIUtil::getEnv();

  jclass clazz = env->FindClass(JAVA_CLASS_REMOTE_SESSION);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  static jmethodID ctor = 0;
  if (ctor == 0)
    {
      ctor = env->GetMethodID(clazz, "<init>", "(J)V");
      if (JNIUtil::isJavaExceptionThrown())
        return;
    }

  jlong cppAddr = this->getCppAddr();

  jobject jremoteSession = env->NewObject(clazz, ctor, cppAddr);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  m_context = new RemoteSessionContext(
      jremoteSession, pool, configDirectory,
      username, password, prompter, jprogress);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  const char* corrected_url = NULL;
  bool cycle_detected = false;
  attempt_set attempted;

  while (retryAttempts-- >= 0)
    {
      SVN_JNI_ERR(
          svn_ra_open4(&m_session, &corrected_url,
                       url, uuid, m_context->getCallbacks(),
                       m_context->getCallbackBaton(),
                       m_context->getConfigData(),
                       pool.getPool()),
                  );

      if (!corrected_url)
        break;

      attempt_insert result = attempted.insert(corrected_url);
      if (!result.second)
        {
          cycle_detected = true;
          break;
        }
    }

  if (cycle_detected)
    {
      jstring exmsg = JNIUtil::makeJString(
          apr_psprintf(pool.getPool(),
                       _("Redirect cycle detected for URL '%s'"),
                       corrected_url));

      jclass excls = env->FindClass(
          JAVA_PACKAGE "/SubversionException");
      if (JNIUtil::isJavaExceptionThrown())
        return;

      static jmethodID exctor = 0;
      if (exctor == 0)
        {
          exctor = env->GetMethodID(excls, "<init>", "(J)V");
          if (JNIUtil::isJavaExceptionThrown())
            return;
        }

      jobject ex = env->NewObject(excls, exctor, exmsg);
      env->Throw(static_cast<jthrowable>(ex));
      return;
    }

  if (corrected_url)
    {
      jstring exmsg = JNIUtil::makeJString(_("Too many redirects"));
      if (JNIUtil::isJavaExceptionThrown())
        return;

      jstring exurl = JNIUtil::makeJString(corrected_url);
      if (JNIUtil::isJavaExceptionThrown())
        return;

      jclass excls = env->FindClass(
          JAVA_PACKAGE "/remote/RetryOpenSession");
      if (JNIUtil::isJavaExceptionThrown())
        return;

      static jmethodID exctor = 0;
      if (exctor == 0)
        {
          exctor = env->GetMethodID(excls, "<init>", "(JJ)V");
          if (JNIUtil::isJavaExceptionThrown())
            return;
        }

      jobject ex = env->NewObject(excls, exctor, exmsg, exurl);
      env->Throw(static_cast<jthrowable>(ex));
      return;
    }

  *jthis_out = jremoteSession;
}

RemoteSession::~RemoteSession()
{
  delete m_context;
}

void
RemoteSession::dispose(jobject jthis)
{
  static jfieldID fid = 0;
  SVNBase::dispose(jthis, &fid, JAVA_CLASS_REMOTE_SESSION);
}

void RemoteSession::reparent(jstring jurl)
{
  JNIStringHolder url(jurl);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  SVN::Pool subPool(pool);
  SVN_JNI_ERR(svn_ra_reparent(m_session, url, subPool.getPool()), );
}

jstring
RemoteSession::getSessionUrl()
{
  SVN::Pool subPool(pool);
  const char* url;
  SVN_JNI_ERR(svn_ra_get_session_url(m_session, &url, subPool.getPool()), NULL);

  jstring jurl = JNIUtil::makeJString(url);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  return jurl;
}

jstring
RemoteSession::getSessionRelativePath(jstring jurl)
{
  JNIStringHolder url(jurl);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  SVN::Pool subPool(pool);
  const char* rel_path;
  SVN_JNI_ERR(svn_ra_get_path_relative_to_session(
                  m_session, &rel_path, url, subPool.getPool()),
              NULL);
  jstring jrel_path = JNIUtil::makeJString(rel_path);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  return jrel_path;
}

jstring
RemoteSession::getReposRelativePath(jstring jurl)
{
  JNIStringHolder url(jurl);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  SVN::Pool subPool(pool);
  const char* rel_path;
  SVN_JNI_ERR(svn_ra_get_path_relative_to_root(m_session, &rel_path, url,
                                               subPool.getPool()),
              NULL);

  jstring jrel_path = JNIUtil::makeJString(rel_path);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  return jrel_path;
}

jstring
RemoteSession::getReposUUID()
{
  SVN::Pool subPool(pool);
  const char * uuid;
  SVN_JNI_ERR(svn_ra_get_uuid2(m_session, &uuid, subPool.getPool()), NULL);

  jstring juuid = JNIUtil::makeJString(uuid);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  return juuid;
}

jstring
RemoteSession::getReposRootUrl()
{
  SVN::Pool subPool(pool);
  const char* url;
  SVN_JNI_ERR(svn_ra_get_repos_root2(m_session, &url, subPool.getPool()),
              NULL);

  jstring jurl = JNIUtil::makeJString(url);
  if (JNIUtil::isJavaExceptionThrown())
    return NULL;

  return jurl;
}

jlong
RemoteSession::getLatestRevision()
{
  SVN::Pool subPool(pool);
  svn_revnum_t rev;
  SVN_JNI_ERR(svn_ra_get_latest_revnum(m_session, &rev, subPool.getPool()),
              SVN_INVALID_REVNUM);
  return rev;
}

jlong
RemoteSession::getRevisionByTimestamp(jlong timestamp)
{
  SVN::Pool subPool(pool);
  svn_revnum_t rev;
  SVN_JNI_ERR(svn_ra_get_dated_revision(m_session, &rev,
                                        apr_time_t(timestamp),
                                        subPool.getPool()),
              SVN_INVALID_REVNUM);
  return rev;
}

namespace {
bool byte_array_to_svn_string(JNIByteArray& ary, svn_string_t& str)
{
  if (ary.isNull())
    return false;

  str.data = reinterpret_cast<const char*>(ary.getBytes());
  str.len = ary.getLength();
  return true;
}
} // anonymous namespace

void
RemoteSession::changeRevisionProperty(
    jlong jrevision, jstring jname,
    jbyteArray jold_value, jbyteArray jvalue)
{
  JNIStringHolder name(jname);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIByteArray old_value(jold_value);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIByteArray value(jvalue);
  if (JNIUtil::isExceptionThrown())
    return;

  svn_string_t str_old_value;
  svn_string_t* const p_old_value = &str_old_value;
  svn_string_t* const* pp_old_value = NULL;
  if (byte_array_to_svn_string(old_value, str_old_value))
      pp_old_value = &p_old_value;

  svn_string_t str_value;
  svn_string_t* p_value = NULL;
  if (byte_array_to_svn_string(value, str_value))
      p_value = &str_value;

  SVN::Pool subPool(pool);
  SVN_JNI_ERR(svn_ra_change_rev_prop2(m_session,
                                      svn_revnum_t(jrevision),
                                      name, pp_old_value, p_value,
                                      subPool.getPool()), );
}

jobject
RemoteSession::getRevisionProperties(jlong jrevision)
{
  SVN::Pool subPool(pool);
  apr_hash_t *props;
  SVN_JNI_ERR(svn_ra_rev_proplist(m_session, svn_revnum_t(jrevision),
                                  &props, subPool.getPool()),
              NULL);

  return CreateJ::PropertyMap(props);
}

jbyteArray
RemoteSession::getRevisionProperty(jlong jrevision, jstring jname)
{
  JNIStringHolder name(jname);
  if (JNIUtil::isExceptionThrown())
    return NULL;

  SVN::Pool subPool(pool);
  svn_string_t *propval;
  SVN_JNI_ERR(svn_ra_rev_prop(m_session, svn_revnum_t(jrevision),
                              name, &propval, subPool.getPool()),
              NULL);

  return JNIUtil::makeJByteArray(propval);
}

jobject
RemoteSession::getLocks(jstring jpath, jobject jdepth)
{
  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return NULL;

  svn_depth_t depth = EnumMapper::toDepth(jdepth);
  if (JNIUtil::isExceptionThrown())
    return NULL;

  SVN::Pool subPool(pool);
  apr_hash_t *locks;
  SVN_JNI_ERR(svn_ra_get_locks2(m_session, &locks, path, depth,
                                subPool.getPool()),
              NULL);

  return CreateJ::LockMap(locks, subPool.getPool());
}

jobject
RemoteSession::checkPath(jstring jpath, jlong jrevision)
{
  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return NULL;

  SVN::Pool subPool(pool);
  svn_node_kind_t kind;
  SVN_JNI_ERR(svn_ra_check_path(m_session, path,
                                svn_revnum_t(jrevision),
                                &kind, subPool.getPool()),
              NULL);

  return EnumMapper::mapNodeKind(kind);
}

jboolean
RemoteSession::hasCapability(jstring jcapability)
{
  JNIStringHolder capability(jcapability);
  if (JNIUtil::isExceptionThrown())
    return false;

  SVN::Pool subPool(pool);
  svn_boolean_t has;
  SVN_JNI_ERR(svn_ra_has_capability(m_session, &has, capability,
                                    subPool.getPool()),
              false);

  return jboolean(has);
}
