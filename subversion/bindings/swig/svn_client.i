/*
 * svn_client.i :  SWIG interface file for svn_client.h
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

%module _client
%include typemaps.i

%import apr.i
%import svn_types.i
%import svn_string.i
%import svn_delta.i

/* -----------------------------------------------------------------------
   don't wrap the following items
*/
%ignore svn_client_proplist_item_s;

/* -----------------------------------------------------------------------
   these types (as 'type **') will always be an OUT param
*/
%apply SWIGTYPE **OUTPARAM {
  svn_client_commit_info_t **
};

/* -----------------------------------------------------------------------
   make return commit item structures magically turn into a 3-tuple of
   (rev, datestring, author).
*/
%typemap(python,argout,fragment="t_output_helper") svn_client_commit_info_t **
{       
    PyObject *list;
    PyObject *rev, *date, *author;
    if (!(*$1)) {
        $result = Py_None;
        Py_INCREF(Py_None);
    }
    list = PyList_New(3);
    rev = PyInt_FromLong((*$1)->revision);
    date = PyString_FromString((*$1)->date);
    author = PyString_FromString((*$1)->author);
    if (!(list && rev && date && author)) {
        Py_XDECREF(rev);
        Py_XDECREF(date);
        Py_XDECREF(author);
        Py_XDECREF(list);
        return NULL;
    }       
    PyList_SET_ITEM(list, 0, rev);
    PyList_SET_ITEM(list, 1, date);
    PyList_SET_ITEM(list, 2, author);
    $result = t_output_helper($result, list);
};

/* -----------------------------------------------------------------------
   all "targets" and "diff_options" arrays are constant inputs of
   svn_stringbuf_t *
 */
%apply const apr_array_header_t *STRINGLIST {
    const apr_array_header_t *targets,
    const apr_array_header_t *diff_options
};

/* -----------------------------------------------------------------------
   fix up the return hash for svn_client_propget()
*/
%apply apr_hash_t **PROPHASH { apr_hash_t **props };

/* -----------------------------------------------------------------------
   handle the return value for svn_client_proplist()
*/

%typemap(ignore) apr_array_header_t ** (apr_array_header_t *temp) {
    $1 = &temp;
}
%typemap(python,argout,fragment="t_output_helper") apr_array_header_t ** {
    svn_client_proplist_item_t **ppitem;
    int i;
    int nelts = (*$1)->nelts;
    PyObject *list = PyList_New(nelts);
    if (list == NULL)
        return NULL;
    ppitem = (svn_client_proplist_item_t **)(*$1)->elts;
    for (i = 0; i < nelts; ++i, ++ppitem) {
        PyObject *item = PyTuple_New(2);
        PyObject *name = PyString_FromStringAndSize((*ppitem)->node_name->data,
                                                    (*ppitem)->node_name->len);
        PyObject *hash = svn_swig_py_prophash_to_dict((*ppitem)->prop_hash);

        if (item == NULL || name == NULL || hash == NULL) {
            Py_XDECREF(item);
            Py_XDECREF(name);
            Py_XDECREF(hash);
            Py_DECREF(list);
            return NULL;
        }
        PyTuple_SET_ITEM(item, 0, name);
        PyTuple_SET_ITEM(item, 1, hash);

        PyList_SET_ITEM(list, i, item);
    }
    $result = t_output_helper($result, list);
}

/* -----------------------------------------------------------------------
   handle svn_wc_notify_func_t/baton pairs
*/

%typemap(python,in) (svn_wc_notify_func_t notify_func, void *notify_baton) {

  $1 = svn_swig_py_notify_func;
  $2 = $input; /* our function is the baton. */
}

/* -----------------------------------------------------------------------
   handle svn_client_get_commit_log_t/baton pairs
*/

%typemap(python,in) (svn_client_get_commit_log_t log_msg_func, 
                     void *log_msg_baton) {

  $1 = svn_swig_py_get_commit_log_func;
  $2 = $input; /* our function is the baton. */
}

/* -----------------------------------------------------------------------
   handle the "statushash" OUTPUT param for svn_client_status()
*/
%typemap(ignore) apr_hash_t **statushash = apr_hash_t **OUTPUT;
%typemap(python,argout,fragment="t_output_helper") apr_hash_t **statushash {
    $result = t_output_helper(
        $result,
        svn_swig_py_convert_hash(*$1, SWIGTYPE_p_svn_wc_status_t));
}

/* -----------------------------------------------------------------------
   We use 'svn_wc_status_t *' in some custom code, but it isn't in the
   API anywhere. Thus, SWIG doesn't generate a typemap entry for it. by
   adding a simple declaration here, SWIG will insert a name for it.
*/
%types(svn_wc_status_t *);

/* ----------------------------------------------------------------------- */

/* Include the headers before we swig-include the svn_client.h header file.
   SWIG will split the nested svn_client_revision_t structure, and we need
   the types declared *before* the split structure is encountered.  */

%header %{
#include "svn_client.h"

#ifdef SWIGPYTHON
#include "swigutil_py.h"
#endif
%}

%include svn_client.h
