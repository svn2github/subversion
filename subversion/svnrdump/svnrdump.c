/*
 *  svnrdump.c: Produce a dumpfile of a local or remote repository
 *  without touching the filesystem, but for temporary files.
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

#include "svn_pools.h"
#include "svn_cmdline.h"
#include "svn_client.h"
#include "svn_hash.h"
#include "svn_ra.h"
#include "svn_repos.h"
#include "svn_path.h"
#include "svn_utf.h"
#include "svn_private_config.h"
#include "svn_string.h"

#include "svnrdump.h"
#include "dump_editor.h"

enum svn_svnrdump__longopt_t
{
   opt_config_dir = SVN_OPT_FIRST_LONGOPT_ID,
   opt_auth_username,
   opt_auth_password,
   opt_non_interactive,
   opt_auth_nocache,
   opt_version,
};

static const apr_getopt_option_t svnrdump__options[] =
{
  {"revision",     'r', 1, N_("REV1[:REV2] range of revisions to dump")},
  {"verbose",      'v', 0, N_("print extra information")},
  {"config-dir",    opt_config_dir, 1, N_("read user configuration files from"
                                          " directory ARG") },
  {"username",      opt_auth_username, 1, N_("specify a username ARG")},
  {"password",      opt_auth_password, 1, N_("specify a password ARG")},
  {"non-interactive", opt_non_interactive, 0, N_("do no interactive"
                                                 " prompting")},
  {"no-auth-cache", opt_auth_nocache, 0, N_("do not cache authentication"
                                             " tokens")},
  
  {"help",          'h', 0, N_("display this help")},
  {"version",       opt_version, 0, N_("show program version information")},

  {0,                  0,   0, 0}
};

static svn_error_t *
replay_revstart(svn_revnum_t revision,
                void *replay_baton,
                const svn_delta_editor_t **editor,
                void **edit_baton,
                apr_hash_t *rev_props,
                apr_pool_t *pool)
{
  struct replay_baton *rb = replay_baton;
  /* First, dump the revision properties. */
  svn_stringbuf_t *propstring;
  svn_stream_t *stdout_stream;
  svn_stream_t *revprop_stream;

  svn_stream_for_stdout(&stdout_stream, pool);

  /* Revision-number: 19 */
  SVN_ERR(svn_stream_printf(stdout_stream, pool,
          SVN_REPOS_DUMPFILE_REVISION_NUMBER
          ": %ld\n", revision));
  propstring = svn_stringbuf_create_ensure(0, pool);
  revprop_stream = svn_stream_from_stringbuf(propstring, pool);
  SVN_ERR(svn_hash_write2(rev_props, revprop_stream, "PROPS-END", pool));
  SVN_ERR(svn_stream_close(revprop_stream));

  /* Prop-content-length: 13 */
  SVN_ERR(svn_stream_printf(stdout_stream, pool,
          SVN_REPOS_DUMPFILE_PROP_CONTENT_LENGTH
          ": %" APR_SIZE_T_FMT "\n", propstring->len));

  /* Content-length: 29 */
  SVN_ERR(svn_stream_printf(stdout_stream, pool,
          SVN_REPOS_DUMPFILE_CONTENT_LENGTH
          ": %" APR_SIZE_T_FMT "\n\n", propstring->len));

  /* Property data. */
  SVN_ERR(svn_stream_write(stdout_stream, propstring->data,
         &(propstring->len)));

  svn_stream_close(stdout_stream);

  /* Extract editor and editor_baton from the replay_baton and
     set them so that the editor callbacks can use them. */
  *editor = rb->editor;
  *edit_baton = rb->edit_baton;

  return SVN_NO_ERROR;
}

static svn_error_t *
replay_revend(svn_revnum_t revision,
              void *replay_baton,
              const svn_delta_editor_t *editor,
              void *edit_baton,
              apr_hash_t *rev_props,
              apr_pool_t *pool)
{
  /* No resources left to free. */
  struct replay_baton *rb = replay_baton;
  if (rb->verbose)
    svn_cmdline_fprintf(stderr, pool, "* Dumped revision %lu\n", revision);
  return SVN_NO_ERROR;
}

/* Return in *SESSION a new RA session to URL.
 * Allocate *SESSION and related data structures in POOL.
 * Use CONFIG_DIR and pass USERNAME, PASSWORD, CONFIG_DIR and
 * NO_AUTH_CACHE to initialize the authorization baton.*/
static svn_error_t *
open_connection(svn_ra_session_t **session,
                const char *url,
                svn_boolean_t non_interactive,
                const char *username,
                const char *password,
                const char *config_dir,
                svn_boolean_t no_auth_cache,
                apr_pool_t *pool)
{
  svn_client_ctx_t *ctx = NULL;
  svn_config_t *cfg_config;

  SVN_ERR(svn_ra_initialize(pool));

  SVN_ERR(svn_config_ensure(config_dir, pool));
  SVN_ERR(svn_client_create_context(&ctx, pool));

  SVN_ERR(svn_config_get_config(&(ctx->config), config_dir, pool));

  cfg_config = apr_hash_get(ctx->config, SVN_CONFIG_CATEGORY_CONFIG,
                            APR_HASH_KEY_STRING);

  /* Default authentication providers for non-interactive use */
  SVN_ERR(svn_cmdline_create_auth_baton(&(ctx->auth_baton), non_interactive,
                username, password, config_dir, no_auth_cache,
                FALSE, cfg_config,
                ctx->cancel_func, ctx->cancel_baton,
                pool));
  SVN_ERR(svn_client_open_ra_session(session, url, ctx, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
replay_range(svn_ra_session_t *session, svn_revnum_t start_revision,
             svn_revnum_t end_revision, apr_pool_t *pool,
             svn_boolean_t verbose)
{
  const svn_delta_editor_t *dump_editor;
  struct replay_baton *replay_baton;
  void *dump_baton;
  const char *uuid;
  svn_stream_t *stdout_stream;

  SVN_ERR(svn_stream_for_stdout(&stdout_stream, pool));

  SVN_ERR(get_dump_editor(&dump_editor, &dump_baton, stdout_stream, pool));

  replay_baton = apr_pcalloc(pool, sizeof(*replay_baton));
  replay_baton->editor = dump_editor;
  replay_baton->edit_baton = dump_baton;
  replay_baton->verbose = verbose;
  SVN_ERR(svn_stream_printf(stdout_stream, pool,
                            SVN_REPOS_DUMPFILE_MAGIC_HEADER ": %d\n\n",
                            SVN_REPOS_DUMPFILE_FORMAT_VERSION));
  SVN_ERR(svn_ra_get_uuid2(session, &uuid, pool));
  SVN_ERR(svn_stream_printf(stdout_stream, pool,
                            SVN_REPOS_DUMPFILE_UUID ": %s\n\n", uuid));
  SVN_ERR(svn_ra_replay_range(session, start_revision, end_revision,
                              0, TRUE, replay_revstart, replay_revend,
                              replay_baton, pool));
  SVN_ERR(svn_stream_close(stdout_stream));
  return SVN_NO_ERROR;
}

static const char *
ensure_appname(const char *progname, apr_pool_t *pool)
{
  if (!progname)
    return "svnrdump";
  
  progname = svn_dirent_internal_style(progname, pool);
  return svn_dirent_basename(progname, NULL);
}

static svn_error_t *
usage(const char *progname, apr_pool_t *pool)
{
  progname = ensure_appname(progname, pool);

  SVN_ERR(svn_cmdline_fprintf(stderr, pool,
                              _("Type '%s --help' for usage.\n"),
                              progname));
  return SVN_NO_ERROR;
}

static svn_error_t *
help(const char *progname, apr_pool_t *pool)
{
  apr_size_t i;

  progname = ensure_appname(progname, pool);

  SVN_ERR(svn_cmdline_printf(
      pool,
      _("usage: %s URL [-r LOWER[:UPPER]]\n\n"
        "Dump the contents of repository at remote URL to stdout in a 'dumpfile'\n"
        "portable format.  Dump revisions LOWER rev through UPPER rev.\n"
        "LOWER defaults to 1 and UPPER defaults to the highest possible revision\n"
        "if omitted.\n"
        "\n"
        "Valid options:\n"),
      progname));

  for (i = 0; svnrdump__options[i].name && svnrdump__options[i].optch; i++)
    {
      const char *optstr;
      svn_opt_format_option(&optstr, svnrdump__options + i, TRUE, pool);
      SVN_ERR(svn_cmdline_fprintf(stdout, pool, "  %s\n", optstr));
    }
  return svn_cmdline_fprintf(stdout, pool, "\n");
}

static svn_error_t *
version(const char *progname, apr_pool_t *pool)
{
  progname = ensure_appname(progname, pool);

  return svn_opt_print_help3(NULL, progname, TRUE, FALSE, NULL,
                             NULL, NULL, NULL, NULL, NULL, pool);
}


/** A statement macro, similar to @c SVN_ERR, but returns an integer.
 *
 * Evaluate @a expr. If it yields an error, handle that error and
 * return @c EXIT_FAILURE.
 */
#define SVNRDUMP_ERR(expr)                                           \
  do {                                                               \
    svn_error_t *svn_err__temp = (expr);                             \
    if (svn_err__temp) {                                             \
      svn_handle_error2(svn_err__temp, stderr, FALSE, "svnrdump: "); \
      svn_error_clear(svn_err__temp);                                \
      return EXIT_FAILURE; }                                         \
  } while (0)



int
main(int argc, const char **argv)
{
  const char *url = NULL;
  char *revision_cut = NULL;
  svn_revnum_t start_revision = svn_opt_revision_unspecified;
  svn_revnum_t end_revision = svn_opt_revision_unspecified;
  svn_boolean_t verbose = FALSE;
  apr_pool_t *pool = NULL;
  svn_ra_session_t *session = NULL;
  const char *config_dir = NULL;
  const char *username = NULL;
  const char *password = NULL;
  svn_boolean_t no_auth_cache = FALSE;
  svn_boolean_t non_interactive = FALSE;
  apr_getopt_t *os;

  if (svn_cmdline_init ("svnrdump", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  pool = svn_pool_create(NULL);

  SVNRDUMP_ERR(svn_cmdline__getopt_init(&os, argc, argv, pool));

  os->interleave = TRUE; /* Options and arguments can be interleaved */

  while (1)
    {
      int opt;
      const char *opt_arg;
      apr_status_t status = apr_getopt_long(os, svnrdump__options, &opt,
                                            &opt_arg);

      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        {
          SVNRDUMP_ERR(usage(argv[0], pool));
          exit(EXIT_FAILURE);
        }

      switch(opt)
        {
          case 'r':
            {
              revision_cut = strchr(opt_arg, ':');
              if (revision_cut)
                {
                  start_revision = (svn_revnum_t)strtoul(opt_arg,
                                                         &revision_cut, 10);
                  end_revision = (svn_revnum_t)strtoul(revision_cut + 1,
                                                       NULL, 10);
                }
              else
                start_revision = (svn_revnum_t)strtoul(opt_arg, NULL, 10);
            }
            break;
          case 'v':
            verbose = TRUE;
            break;
          case opt_config_dir:
            config_dir = opt_arg;
            break;
          case opt_version:
            SVNRDUMP_ERR(version(argv[0], pool));
            exit(EXIT_SUCCESS);
            break;
          case 'h':
            SVNRDUMP_ERR(help(argv[0], pool));
            exit(EXIT_SUCCESS);
            break;
          case opt_auth_username:
            SVNRDUMP_ERR(svn_utf_cstring_to_utf8(&username, opt_arg, pool));
            break;
          case opt_auth_password:
            SVNRDUMP_ERR(svn_utf_cstring_to_utf8(&password, opt_arg, pool));
            break;
          case opt_auth_nocache:
            no_auth_cache = TRUE;
            break;
          case opt_non_interactive:
            non_interactive = TRUE;
            break;
        }
    }

  /* Only continue if the only not option argument is a url, to allow
     implementing 'svnrdump dump URL' like handling later without breaking
     backward compatibility */
  if ((os->ind != os->argc-1)
      || !svn_path_is_url(os->argv[os->ind]))
    {
      SVNRDUMP_ERR(usage(argv[0], pool));
      exit(EXIT_FAILURE);
    }

  SVNRDUMP_ERR(svn_utf_cstring_to_utf8(&url, os->argv[os->ind], pool));

  url = svn_uri_canonicalize(os->argv[os->ind], pool);


  SVNRDUMP_ERR(open_connection(&session,
                               url,
                               non_interactive,
                               username,
                               password,
                               config_dir,
                               no_auth_cache,
                               pool));

  /* Have sane start_revision and end_revision defaults if unspecified */
  if (start_revision == svn_opt_revision_unspecified)
    start_revision = 1;
  if (end_revision == svn_opt_revision_unspecified)
    SVNRDUMP_ERR(svn_ra_get_latest_revnum(session, &end_revision, pool));

  SVNRDUMP_ERR(replay_range(session, start_revision, end_revision, pool, verbose));

  svn_pool_destroy(pool);

  return 0;
}
