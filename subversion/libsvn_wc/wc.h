/*
 * wc.h :  shared stuff internal to the svn_wc library.
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


#include <apr_pools.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_wc.h"



#define SVN_WC__DIFF_EXT      ".diff"
#define SVN_WC__TMP_EXT       ".tmp"
#define SVN_WC__TEXT_REJ_EXT  ".rej"
#define SVN_WC__PROP_REJ_EXT  ".prej"



/** File comparisons **/

/* Set *SAME to non-zero if file1 and file2 have the same contents,
   else set it to zero. */
svn_error_t *svn_wc__files_contents_same_p (svn_boolean_t *same,
                                            svn_stringbuf_t *file1,
                                            svn_stringbuf_t *file2,
                                            apr_pool_t *pool);


/* A special timestamp value which means "use the timestamp from the
   working copy".  This is sometimes used in a log entry like:
   
   <modify-entry name="foo.c" revision="5" timestamp="working"/>

 */
#define SVN_WC_TIMESTAMP_WC   "working"


/* The name of the '.svnignore' file */
#define SVN_WC_SVNIGNORE      ".svnignore"



/*** Locking. ***/

/* Lock the working copy administrative area.
   Wait for WAIT_FOR seconds if encounter another lock, trying again every
   second, then return 0 if success or an SVN_ERR_WC_LOCKED error if
   failed to obtain the lock. */
svn_error_t *svn_wc__lock (svn_stringbuf_t *path, int wait_for, apr_pool_t *pool);

/* Unlock PATH, or error if can't. */
svn_error_t *svn_wc__unlock (svn_stringbuf_t *path, apr_pool_t *pool);

/* Set *LOCKED to non-zero if PATH is locked, else set it to zero. */
svn_error_t *svn_wc__locked (svn_boolean_t *locked, 
                             svn_stringbuf_t *path,
                             apr_pool_t *pool);


/*** Names and file/dir operations in the administrative area. ***/

/* Create DIR as a working copy directory. */
svn_error_t *svn_wc__set_up_new_dir (svn_stringbuf_t *path,
                                     svn_stringbuf_t *ancestor_path,
                                     svn_revnum_t ancestor_revnum,
                                     apr_pool_t *pool);


/* kff todo: namespace-protecting these #defines so we never have to
   worry about them conflicting with future all-caps symbols that may
   be defined in svn_wc.h. */

/** The files within the administrative subdir. **/
#define SVN_WC__ADM_FORMAT              "format"
#define SVN_WC__ADM_README              "README"
#define SVN_WC__ADM_ENTRIES             "entries"
#define SVN_WC__ADM_LOCK                "lock"
#define SVN_WC__ADM_TMP                 "tmp"
#define SVN_WC__ADM_TEXT_BASE           "text-base"
#define SVN_WC__ADM_PROPS               "props"
#define SVN_WC__ADM_PROP_BASE           "prop-base"
#define SVN_WC__ADM_DIR_PROPS           "dir-props"
#define SVN_WC__ADM_DIR_PROP_BASE       "dir-prop-base"
#define SVN_WC__ADM_WCPROPS             "wcprops"
#define SVN_WC__ADM_DIR_WCPROPS         "dir-wcprops"
#define SVN_WC__ADM_LOG                 "log"
#define SVN_WC__ADM_KILLME              "KILLME"
#define SVN_WC__ADM_AUTH_DIR            "auth"


/* The basename of the ".prej" file, if a directory ever has property
   conflicts.  This .prej file will appear *within* the conflicted
   directory.  */
#define SVN_WC__THIS_DIR_PREJ           "dir_conflicts"

/* Return a string containing the admin subdir name. */
svn_stringbuf_t *svn_wc__adm_subdir (apr_pool_t *pool);


/* Return a path to something in PATH's administrative area.
 * Return path to the thing in the tmp area if TMP is non-zero.
 * Varargs are (const char *)'s, the final one must be NULL.
 */
svn_stringbuf_t * svn_wc__adm_path (svn_stringbuf_t *path,
                                    svn_boolean_t tmp,
                                    apr_pool_t *pool,
                                    ...);

/* Return TRUE if a thing in the administrative area exists, FALSE
   otherwise. */
svn_boolean_t svn_wc__adm_path_exists (svn_stringbuf_t *path,
                                       svn_boolean_t tmp,
                                       apr_pool_t *pool,
                                       ...);


/* Make `PATH/<adminstrative_subdir>/THING'. */
svn_error_t *svn_wc__make_adm_thing (svn_stringbuf_t *path,
                                     const char *thing,
                                     int type,
                                     apr_fileperms_t perms,
                                     svn_boolean_t tmp,
                                     apr_pool_t *pool);

/* Cleanup the temporary storage area of the administrative
   directory. */
svn_error_t *svn_wc__adm_cleanup_tmp_area (svn_stringbuf_t *path, 
                                           apr_pool_t *pool);




/*** Opening all kinds of adm files ***/

/* Yo, read this if you open and close files in the adm area:
 *
 * When you open a file for writing with svn_wc__open_foo(), the file
 * is actually opened in the corresponding location in the tmp/
 * directory (and if you're appending as well, then the tmp file
 * starts out as a copy of the original file). 
 *
 * Somehow, this tmp file must eventually get renamed to its real
 * destination in the adm area.  You can do it either by passing the
 * SYNC flag to svn_wc__close_foo(), or by calling
 * svn_wc__sync_foo() (though of course you should still have
 * called svn_wc__close_foo() first, just without the SYNC flag).
 *
 * In other words, the adm area is only capable of modifying files
 * atomically, but you get some control over when the rename happens.
 */

/* Open `PATH/<adminstrative_subdir>/FNAME'. */
svn_error_t *svn_wc__open_adm_file (apr_file_t **handle,
                                    const svn_stringbuf_t *path,
                                    const char *fname,
                                    apr_int32_t flags,
                                    apr_pool_t *pool);


/* Close `PATH/<adminstrative_subdir>/FNAME'. */
svn_error_t *svn_wc__close_adm_file (apr_file_t *fp,
                                     const svn_stringbuf_t *path,
                                     const char *fname,
                                     int sync,
                                     apr_pool_t *pool);

/* Remove `PATH/<adminstrative_subdir>/THING'. */
svn_error_t *svn_wc__remove_adm_file (svn_stringbuf_t *path,
                                      apr_pool_t *pool,
                                      ...);

/* Open the text-base for FILE.
 * FILE can be any kind of path ending with a filename.
 * Behaves like svn_wc__open_adm_file(), which see.
 */
svn_error_t *svn_wc__open_text_base (apr_file_t **handle,
                                     svn_stringbuf_t *file,
                                     apr_int32_t flags,
                                     apr_pool_t *pool);

/* Close the text-base for FILE.
 * FP was obtained from svn_wc__open_text_base().
 * Behaves like svn_wc__close_adm_file(), which see.
 */
svn_error_t *svn_wc__close_text_base (apr_file_t *fp,
                                      svn_stringbuf_t *file,
                                      int sync,
                                      apr_pool_t *pool);


/* Open FILE in the authentication area of PATH's administratve area.
 * FILE is a single file's name, i.e. a basename.
 * Behaves like svn_wc__open_adm_file(), which see.
 */
svn_error_t *svn_wc__open_auth_file (apr_file_t **handle,
                                     svn_stringbuf_t *path,
                                     svn_stringbuf_t *auth_filename,
                                     apr_int32_t flags,
                                     apr_pool_t *pool);

/* Close the authentication FILE in PATH's administrative area.
 * FP was obtained from svn_wc__open_auth_file().
 * Behaves like svn_wc__close_adm_file(), which see.
 */
svn_error_t *svn_wc__close_auth_file (apr_file_t *handle,
                                      svn_stringbuf_t *path,
                                      svn_stringbuf_t *file,
                                      int sync,
                                      apr_pool_t *pool);

/* Open the property file for PATH.
 * PATH can be any kind of path, either file or dir.
 *
 * If BASE is set, then the "pristine" property file will be opened.
 * If WCPROPS is set, then the "wc" property file will be opened.
 *
 * (Don't set BASE and WCPROPS at the same time; this is meaningless.)
 */
svn_error_t *svn_wc__open_props (apr_file_t **handle,
                                 svn_stringbuf_t *path,
                                 apr_int32_t flags,
                                 svn_boolean_t base,
                                 svn_boolean_t wcprops,
                                 apr_pool_t *pool);

/* Close the property file for PATH.
 * FP was obtained from svn_wc__open_props().
 *
 * The BASE and WCPROPS must have the same state used to open the file!
 *
 * Like svn_wc__close_adm_file(), SYNC indicates the file should be
 * atomically written.
 */
svn_error_t *svn_wc__close_props (apr_file_t *fp,
                                  svn_stringbuf_t *path,
                                  svn_boolean_t base,
                                  svn_boolean_t wcprops,
                                  int sync,
                                  apr_pool_t *pool);

/* Atomically rename a temporary property file to its canonical
   location.  The tmp file should be closed already. 

   Again, BASE and WCPROPS flags should be identical to those used to
   open the file. */
svn_error_t *
svn_wc__sync_props (svn_stringbuf_t *path, 
                    svn_boolean_t base,
                    svn_boolean_t wcprops,
                    apr_pool_t *pool);


/* Atomically rename a temporary text-base file to its canonical
   location.  The tmp file should be closed already. */
svn_error_t *
svn_wc__sync_text_base (svn_stringbuf_t *path, apr_pool_t *pool);


/* Return a path to PATH's text-base file.
   If TMP is set, return a path to the tmp text-base file. */
svn_stringbuf_t *svn_wc__text_base_path (const svn_stringbuf_t *path,
                                         svn_boolean_t tmp,
                                         apr_pool_t *pool);


/* Set *PROP_PATH to PATH's working properties file.
   If TMP is set, return a path to the tmp working property file. 
   PATH can be a directory or file, and even have changed w.r.t. the
   working copy's adm knowledge. */
svn_error_t *svn_wc__prop_path (svn_stringbuf_t **prop_path,
                                const svn_stringbuf_t *path,
                                svn_boolean_t tmp,
                                apr_pool_t *pool);


/* Set *PROP_PATH to PATH's `pristine' properties file.
   If TMP is set, return a path to the tmp working property file. 
   PATH can be a directory or file, and even have changed w.r.t. the
   working copy's adm knowledge. */
svn_error_t *svn_wc__prop_base_path (svn_stringbuf_t **prop_path,
                                     const svn_stringbuf_t *path,
                                     svn_boolean_t tmp,
                                     apr_pool_t *pool);


/* Return a path to the 'wcprop' file for PATH, possibly in TMP area.  */
svn_error_t *svn_wc__wcprop_path (svn_stringbuf_t **wcprop_path,
                                  const svn_stringbuf_t *path,
                                  svn_boolean_t tmp,
                                  apr_pool_t *pool);


/* Ensure that PATH is a locked working copy directory.
 *
 * (In practice, this means creating an adm area if none exists, in
 * which case it is locked from birth, or else locking an adm area
 * that's already there.)
 * 
 * REPOSITORY is a repository string for initializing the adm area.
 *
 * REVISION is the revision for this directory.  kff todo: ancestor_path?
 */
svn_error_t *svn_wc__ensure_wc (svn_stringbuf_t *path,
                                svn_stringbuf_t *ancestor_path,
                                svn_revnum_t ancestor_revision,
                                apr_pool_t *pool);


/* Ensure that an administrative area exists for PATH, so that PATH is
 * a working copy subdir.
 *
 * Use REPOSITORY for the wc's repository.
 *
 * Does not ensure existence of PATH itself; if PATH does not exist,
 * an error will result. 
 */
svn_error_t *svn_wc__ensure_adm (svn_stringbuf_t *path,
                                 svn_stringbuf_t *ancestor_path,
                                 svn_revnum_t ancestor_revision,
                                 apr_pool_t *pool);


/* Blow away the admistrative directory associated with directory
   PATH, make sure beforehand that it isn't locked. */
svn_error_t *svn_wc__adm_destroy (svn_stringbuf_t *path,
                                  apr_pool_t *pool);


/*** The log file. ***/

/* Note: every entry in the logfile is either idempotent or atomic.
 * This allows us to remove the entire logfile when every entry in it
 * has been completed -- if you crash in the middle of running a
 * logfile, and then later are running over it again as part of the
 * recovery, a given entry is "safe" in the sense that you can either
 * tell it has already been done (in which case, ignore it) or you can
 * do it again without ill effect.
 */

/** Log actions. **/

/* Set some attributes on SVN_WC__LOG_ATTR_NAME's entry.  Unmentioned
   attributes are unaffected. */
#define SVN_WC__LOG_MODIFY_ENTRY        "modify-entry"

/* Delete the entry SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_DELETE_ENTRY        "delete-entry"

/* Run an external command:
 *    - command to run is SVN_WC__LOG_ATTR_NAME
 *    - arguments are SVN_WC__LOG_ATTR_ARG_[1,2,3,...]
 *    - input from SVN_WC__LOG_ATTR_INFILE, defaults to stdin
 *    - output into SVN_WC__LOG_ATTR_OUTFILE, defaults to stdout
 *    - stderr into SVN_WC__LOG_ATTR_ERRFILE, defaults to stderr
 *
 * The program will be run in the working copy directory, that is, the
 * same directory from which paths in the log file are rooted.
 */
#define SVN_WC__LOG_RUN_CMD             "run"

/* Move file SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST. */
#define SVN_WC__LOG_MV                  "mv"

/* Copy file SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST. */
#define SVN_WC__LOG_CP                  "cp"

/* Remove file SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_RM                  "rm"

/* If SVN_WC__LOG_ATTR_TEXT_REJFILE is 0 bytes, remove it.  Otherwise
   mark SVN_WC__LOG_ATTR_NAME's entry as being in a state of
   conflict. */
#define SVN_WC__LOG_DETECT_CONFLICT         "detect-conflict"

/* Append file from SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST. */
#define SVN_WC__LOG_APPEND              "append"


/* Handle closure after an update completes successfully:  
 *
 *   If SVN_WC__LOG_ATTR_TEXT_REJFILE exists and has >0 size, then
 *   mark the entry as textually conflicted; else remove a 0 byte
 *   reject file.
 *
 *   Similarly for SVN_WC__LOG_ATTR_PROP_REJFILE.
 */
#define SVN_WC__LOG_UPDATED            "updated"

/* Handle closure after a commit completes successfully:  
 *
 *   If SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME exists, then
 *      compare SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME with working file
 *         if they're the same, use working file's timestamp
 *         else use SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME's timestamp
 *      set SVN_WC__LOG_ATTR_NAME's revision to N
 */
#define SVN_WC__LOG_COMMITTED           "committed"

/** Log attributes. **/
#define SVN_WC__LOG_ATTR_NAME           "name"
#define SVN_WC__LOG_ATTR_DEST           "dest"
#define SVN_WC__LOG_ATTR_REVISION       "revision"
#define SVN_WC__LOG_ATTR_TEXT_REJFILE   "text-rejfile"
#define SVN_WC__LOG_ATTR_PROP_REJFILE   "prop-rejfile"
/* The rest are for SVN_WC__LOG_RUN_CMD.  Extend as necessary. */
#define SVN_WC__LOG_ATTR_INFILE         "infile"
#define SVN_WC__LOG_ATTR_OUTFILE        "outfile"
#define SVN_WC__LOG_ATTR_ERRFILE        "errfile"
#define SVN_WC__LOG_ATTR_ARG_1          "arg1"
#define SVN_WC__LOG_ATTR_ARG_2          "arg2"
#define SVN_WC__LOG_ATTR_ARG_3          "arg3"
#define SVN_WC__LOG_ATTR_ARG_4          "arg4"
#define SVN_WC__LOG_ATTR_ARG_5          "arg5"
#define SVN_WC__LOG_ATTR_ARG_6          "arg6"
#define SVN_WC__LOG_ATTR_ARG_7          "arg7"
#define SVN_WC__LOG_ATTR_ARG_8          "arg8"
#define SVN_WC__LOG_ATTR_ARG_9          "arg9"


/* Starting at PATH, write out log entries indicating that a commit
 * succeeded, using REVISION as the new revision number.  run_log will
 * use these log items to complete the commit. 
 * 
 * Targets is a hash of files/dirs that actually got committed --
 * these are the only ones who we can write log items for, and whose
 * revision numbers will get set.  todo: eventually this hash will be
 * of the sort used by svn_wc__compose_paths(), as with all entries
 * recursers.
 */
svn_error_t *svn_wc__log_commit (svn_stringbuf_t *path,
                                 apr_hash_t *targets,
                                 svn_revnum_t revision,
                                 apr_pool_t *pool);


/* Process the instructions in the log file for PATH. */
svn_error_t *svn_wc__run_log (svn_stringbuf_t *path, apr_pool_t *pool);



/*** Handling the `entries' file. ***/

#define SVN_WC__ENTRIES_TOPLEVEL       "wc-entries"
#define SVN_WC__ENTRIES_ENTRY          "entry"

/* String representations for svn_node_kind.  This maybe should be
   abstracted farther out? */
#define SVN_WC__ENTRIES_ATTR_FILE_STR   "file"
#define SVN_WC__ENTRIES_ATTR_DIR_STR    "dir"

/* Initialize contents of `entries' for a new adm area. */
svn_error_t *svn_wc__entries_init (svn_stringbuf_t *path,
                                   svn_stringbuf_t *ancestor_path,
                                   apr_pool_t *pool);


/* Create or overwrite an `entries' file for PATH using the contents
   of ENTRIES.  See also svn_wc_entries_read() in the public api. */
svn_error_t *svn_wc__entries_write (apr_hash_t *entries,
                                    svn_stringbuf_t *path,
                                    apr_pool_t *pool);


/* Create a new entry from the attributes hash ATTS.  Use POOL for all
   allocations, EXCEPT that **NEW_ENTRY->attributes will simply point
   to the ATTS passed to the function -- this hash is not copied into
   a new pool!  MODIFY_FLAGS are set to the fields that were
   explicitly modified by the attribute hash.  */
svn_error_t *svn_wc__atts_to_entry (svn_wc_entry_t **new_entry,
                                    apr_uint16_t *modify_flags,
                                    apr_hash_t *atts,
                                    apr_pool_t *pool);

/* Your one-stop shopping for changing an entry:

   For PATH's entries file, create or modify an entry NAME by folding
   (merging) changes into it.

   Use MODIFY_FLAGS to denote which of the following function
   arguments to pay attention to and how to interpret those arguments.
   If the flag that corresponds to the argument is not passed to the
   function, that field in the entry structure will not be modified at
   all.  Note that in the descriptions below, the "valid values"
   restrictions on a field only apply when that field is being
   "noticed" via the MODIFY_FLAGS.
 
   - REVISION is the entry's revision number.  

   - KIND is the node type of this entry.  Valid values are
     svn_node_dir and _file.

   - SCHEDULE is the scheduled action pending on this entry.  When the
     _MODIFY_FORCE flag is set, valid values are
     svn_wc_schedule_normal, _add, _delete, and _replace.

   - EXISTENCE is the current state of existence of this entry.

   - CONFLICTED reflects whether or not this entry stands in a state
     of conflict with the repository.

   - TEXT_TIME is the entry's textual timestamp.

   - PROP_TIME is the entry's property timestamp.

   - ATTRIBUTES is a hash of properties on the entry.  The keys are
     (const char *) and the values are (svn_stringbuf_t *).  These
     overwrite where they collide with existing attributes.
     
   Remaining (const char *) arguments are attributes to be removed
   from the entry, terminated by a final NULL.  These will be removed
   even if they also appear in ATTS.
     
   NOTE: when you call this function, the entries file will be read,
   tweaked, and written back out.  */

/* The MODIFY_FLAGS to note which of the parameters to pay attention to are
   as follows: */
#define SVN_WC__ENTRY_MODIFY_REVISION      0x0001
#define SVN_WC__ENTRY_MODIFY_KIND          0x0002
#define SVN_WC__ENTRY_MODIFY_SCHEDULE      0x0004
#define SVN_WC__ENTRY_MODIFY_EXISTENCE     0x0008
#define SVN_WC__ENTRY_MODIFY_CONFLICTED    0x0010
#define SVN_WC__ENTRY_MODIFY_TEXT_TIME     0x0020
#define SVN_WC__ENTRY_MODIFY_PROP_TIME     0x0040
#define SVN_WC__ENTRY_MODIFY_ATTRIBUTES    0x0080

/* or perhaps this to mean all of those above... */
#define SVN_WC__ENTRY_MODIFY_ALL           0x7FFF

/* ORed together with this to mean "I really mean this, don't be
   trying to protect me from myself on this one." */
#define SVN_WC__ENTRY_MODIFY_FORCE         0x8000

svn_error_t *svn_wc__entry_modify (svn_stringbuf_t *path,
                                   svn_stringbuf_t *name,
                                   apr_uint16_t modify_flags,
                                   svn_revnum_t revision,
                                   enum svn_node_kind kind,
                                   enum svn_wc_schedule_t schedule,
                                   enum svn_wc_existence_t existence,
                                   svn_boolean_t conflicted,
                                   apr_time_t text_time,
                                   apr_time_t prop_time,
                                   apr_hash_t *attributes,
                                   apr_pool_t *pool,
                                   ...);

/* Remove entry NAME from ENTRIES, unconditionally. */
void svn_wc__entry_remove (apr_hash_t *entries, svn_stringbuf_t *name);


/* Return a duplicate of ENTRY, allocated in POOL.  No part of the new
   entry will be shared with ENTRY. */
svn_wc_entry_t *svn_wc__entry_dup (svn_wc_entry_t *entry, apr_pool_t *pool);




/*** General utilities that may get moved upstairs at some point. */

/* Ensure that DIR exists. */
svn_error_t *svn_wc__ensure_directory (svn_stringbuf_t *path, apr_pool_t *pool);


/* Ensure that every file or dir underneath PATH is at REVISION.  If
   not, bump it to exactly that value.  (Used at the end of an
   update.) */
svn_error_t *svn_wc__ensure_uniform_revision (svn_stringbuf_t *path,
                                              svn_revnum_t revision,
                                              apr_pool_t *pool);



/*** Routines that deal with properties ***/

/* Given two property hashes (working copy and `base'), deduce what
   propchanges the user has made since the last update.  Return these
   changes as a series of (svn_prop_t *) objects stored in
   LOCAL_PROPCHANGES, allocated from POOL.  */
svn_error_t *
svn_wc__get_local_propchanges (apr_array_header_t **local_propchanges,
                               apr_hash_t *localprops,
                               apr_hash_t *baseprops,
                               apr_pool_t *pool);



/* Given two propchange objects, return TRUE iff they conflict.  If
   there's a conflict, DESCRIPTION will contain an english description
   of the problem. */

/* For note, here's the table being implemented:

              |  update set     |    update delete   |
  ------------|-----------------|--------------------|
  user set    | conflict iff    |      conflict      |
              |  vals differ    |                    |
  ------------|-----------------|--------------------|
  user delete |   conflict      |      merge         |
              |                 |    (no problem)    |
  ----------------------------------------------------

*/
svn_boolean_t
svn_wc__conflicting_propchanges_p (svn_stringbuf_t **description,
                                   svn_prop_t *local,
                                   svn_prop_t *update,
                                   apr_pool_t *pool);

/* Look up the entry NAME within PATH and see if it has a `current'
   reject file describing a state of conflict.  If such a file exists,
   return the name of the file in REJECT_FILE.  If no such file exists,
   return (REJECT_FILE = NULL). */
svn_error_t *
svn_wc__get_existing_prop_reject_file (svn_stringbuf_t **reject_file,
                                       svn_stringbuf_t *path,
                                       const svn_stringbuf_t *name,
                                       apr_pool_t *pool);

/* If PROPFILE_PATH exists (and is a file), assume it's full of
   properties and load this file into HASH.  Otherwise, leave HASH
   untouched.  */
svn_error_t *
svn_wc__load_prop_file (svn_stringbuf_t *propfile_path,
                        apr_hash_t *hash,
                        apr_pool_t *pool);



/* Given a HASH full of property name/values, write them to a file
   located at PROPFILE_PATH */
svn_error_t *
svn_wc__save_prop_file (svn_stringbuf_t *propfile_path,
                        apr_hash_t *hash,
                        apr_pool_t *pool);


/* Given PATH/NAME and an array of PROPCHANGES, merge the changes into
   the working copy.  Necessary log entries will be appended to
   ENTRY_ACCUM.

   If we are attempting to merge changes to a directory, simply pass
   the directory as PATH and NULL for NAME.

   If conflicts are found when merging, they are placed into a
   temporary .prej file within SVN. Log entries are then written to
   move this file into PATH, or to append the conflicts to the file's
   already-existing .prej file in PATH.  */
svn_error_t *
svn_wc__do_property_merge (svn_stringbuf_t *path,
                           const svn_stringbuf_t *name,
                           apr_array_header_t *propchanges,
                           apr_pool_t *pool,
                           svn_stringbuf_t **entry_accum);


/* Get a single 'wcprop' NAME for versioned object PATH, return in
   *VALUE. */
svn_error_t *svn_wc__wcprop_get (svn_stringbuf_t **value,
                                 svn_stringbuf_t *name,
                                 svn_stringbuf_t *path,
                                 apr_pool_t *pool);

/* Set a single 'wcprop' NAME to VALUE for versioned object PATH. */
svn_error_t * svn_wc__wcprop_set (svn_stringbuf_t *name,
                                  svn_stringbuf_t *value,
                                  svn_stringbuf_t *path,
                                  apr_pool_t *pool);


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

