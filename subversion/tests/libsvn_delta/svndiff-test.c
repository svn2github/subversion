/* svndiff-test.c -- test driver for text deltas
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <apr_general.h>
#include "svn_base64.h"
#include "svn_quoprint.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_error.h"


int
main (int argc, char **argv)
{
  apr_status_t apr_err;
  apr_file_t *source_file;
  apr_file_t *target_file;
  apr_file_t *stout;
  svn_txdelta_stream_t *txdelta_stream;
  svn_txdelta_window_handler_t svndiff_handler;
  svn_stream_t *encoder;
  void *svndiff_baton;
  apr_pool_t *pool = svn_pool_create (NULL);

  if (argc < 3)
    {
      printf ("usage: %s source target\n", argv[0]);
      exit (0);
    }

  apr_err = apr_file_open (&source_file, argv[1], (APR_READ | APR_BINARY),
                           APR_OS_DEFAULT, pool);
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    {
      fprintf (stderr, "unable to open \"%s\" for reading\n", argv[1]);
      exit (1);
    }

  apr_err = apr_file_open (&target_file, argv[2], (APR_READ | APR_BINARY),
                           APR_OS_DEFAULT, pool);
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    {
      fprintf (stderr, "unable to open \"%s\" for reading\n", argv[2]);
      exit (1);
    }

  apr_initialize();
  svn_txdelta (&txdelta_stream,
               svn_stream_from_aprfile (source_file, pool),
	       svn_stream_from_aprfile (target_file, pool),
               pool);

  apr_err = apr_file_open_stdout (&stout, pool);
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    {
      fprintf (stderr, "unable to open stdout for writing\n");
      exit (1);
    }

#ifdef QUOPRINT_SVNDIFFS
  encoder = svn_quoprint_encode (svn_stream_from_aprfile (stout, pool), pool);
#else
  encoder = svn_base64_encode (svn_stream_from_aprfile (stout, pool), pool);
#endif
  svn_txdelta_to_svndiff (encoder, pool, &svndiff_handler, &svndiff_baton);
  svn_txdelta_send_txstream (txdelta_stream,
                             svndiff_handler,
                             svndiff_baton,
                             pool);

  apr_file_close (source_file);
  apr_file_close (target_file);
  svn_pool_destroy (pool);
  apr_terminate();
  exit (0);
}
