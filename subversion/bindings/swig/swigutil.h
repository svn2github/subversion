/*
 * swigutil.h :  utility functions and stuff for the SWIG bindings
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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


#ifndef SWIGUTIL_H
#define SWIGUTIL_H

#include <apr.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_string.h"

#ifdef SWIGPYTHON
#include <Python.h>
#endif


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/* This macro can be placed into a wrapper file for ensuring that a
   type will be registered with SWIG. */
#define MAKE_TYPE_IMPL(type) \
	struct type; \
	static void _ignore_##type(struct type *arg) { }


/* This file is being included outside of a wrapper file. We need to stub
   out some of the SWIG types. */

/* if SWIGEXPORT is defined, then we're in a wrapper. otherwise, we need
   the prototypes and type definitions. */
#ifndef SWIGEXPORT
#define SVN_NEED_SWIG_TYPES
#endif

#ifdef SVN_NEED_SWIG_TYPES

typedef struct _unnamed swig_type_info;
PyObject *SWIG_NewPointerObj(void *, swig_type_info *);

#endif /* SVN_NEED_SWIG_TYPES */


#ifdef SWIGPYTHON

/* helper function to convert an apr_hash_t* (char* -> svnstring_t*) to
   a Python dict */
PyObject *svn_swig_prophash_to_dict(apr_hash_t *hash);

/* convert a hash of 'const char *' -> TYPE into a Python dict */
PyObject *svn_swig_convert_hash(apr_hash_t *hash, swig_type_info *type);

/* helper function to convert a 'char ***' into a Python list of string
   objects */
PyObject *svn_swig_c_strings_to_list(char **strings);


#endif /* SWIGPYTHON */



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SWIGUTIL_H */

/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end:
 */
