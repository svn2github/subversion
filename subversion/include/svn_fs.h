/* svn_fs.h :  interface to the Subversion filesystem
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

/* ==================================================================== */


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef SVN_FS_H
#define SVN_FS_H

#include <apr_pools.h>
#include <apr_hash.h>
#include "svn_types.h"
#include "svn_error.h"
#include "svn_delta.h"
#include "svn_io.h"



/* Opening and creating filesystems.  */


/* An object representing a Subversion filesystem.  */
typedef struct svn_fs_t svn_fs_t;


/* Create a new filesystem object in POOL.  It doesn't refer to any
   actual repository yet; you need to invoke svn_fs_open_* or
   svn_fs_create_* on it for that to happen.  */
svn_fs_t *svn_fs_new (apr_pool_t *pool);


/* Free the filesystem object FS.  This frees memory, closes files,
   frees database library structures, etc.  */
svn_error_t *svn_fs_close_fs (svn_fs_t *fs);


/* The type of a warning callback function.  BATON is the value specified
   in the call to `svn_fs_set_warning_func'; the filesystem passes it through
   to the callback.  FMT is a printf-style format string, which tells us
   how to interpret any successive arguments.  */
typedef void (svn_fs_warning_callback_t) (void *baton, const char *fmt, ...);


/* Provide a callback function, WARNING, that FS should use to report
   warning messages.  To print a warning message, the filesystem will
   call WARNING, passing it BATON, a printf-style format string, and
   any further arguments as appropriate for the format string.

   If it's acceptable to print messages on stderr, then the function
   `svn_handle_warning', declared in "svn_error.h", would be a
   suitable warning function.

   By default, this is set to a function that will crash the process.
   Dumping to stderr or /dev/tty is not acceptable default behavior
   for server processes, since those may both be equivalent to
   /dev/null.  */
void svn_fs_set_warning_func (svn_fs_t *fs,
			      svn_fs_warning_callback_t *warning,
			      void *warning_baton);



/* Subversion filesystems based on Berkeley DB.  */

/* There are many possible ways to implement the Subversion filesystem
   interface.  You could implement it directly using ordinary POSIX
   filesystem operations; you could build it using an SQL server as a
   back end; you could build it on RCS; and so on.

   The functions on this page create filesystem objects that use
   Berkeley DB (http://www.sleepycat.com) to store their data.
   Berkeley DB supports transactions and recoverability, making it
   well-suited for Subversion.

   A Berkeley DB ``environment'' is a Unix directory containing
   database files, log files, backing files for shared memory buffers,
   and so on --- everything necessary for a complex database
   application.  Each Subversion filesystem lives in a single Berkeley
   DB environment.  */


/* Create a new, empty Subversion filesystem, stored in a Berkeley DB
   environment named ENV.  Make FS refer to this new filesystem.
   FS provides the memory pool, warning function, etc.  */
svn_error_t *svn_fs_create_berkeley (svn_fs_t *fs, const char *env);


/* Make FS refer to the Subversion filesystem stored in the Berkeley
   DB environment ENV.  ENV must refer to a file or directory created
   by `svn_fs_create_berkeley'.

   Only one thread may operate on any given filesystem object at once.
   Two threads may access the same filesystem simultaneously only if
   they open separate filesystem objects.  */
svn_error_t *svn_fs_open_berkeley (svn_fs_t *fs, const char *env);


/* Perform any necessary non-catastrophic recovery on a Berkeley
   DB-based Subversion filesystem, stored in the environment ENV.  Do
   any necessary allocation within POOL.

   After an unexpected server exit, due to a server crash or a system
   crash, a Subversion filesystem based on Berkeley DB needs to run
   recovery procedures to bring the database back into a consistent
   state and release any locks that were held by the deceased process.
   The recovery procedures require exclusive access to the database
   --- while they execute, no other process or thread may access the
   database.

   In a server with multiple worker processes, like Apache, if a
   worker process accessing the filesystem dies, you must stop the
   other worker processes, and run recovery.  Then, the other worker
   processes can re-open the database and resume work.

   If the server exited cleanly, there is no need to run recovery, but
   there is no harm in it, either, and it take very little time.  So
   it's a fine idea to run recovery when the server process starts,
   before it begins handling any requests.  */

svn_error_t *svn_fs_berkeley_recover (const char *path,
				      apr_pool_t *pool);



/* Node and Node Revision ID's.  */

/* In a Subversion filesystem, a `node' corresponds roughly to an
   `inode' in a Unix filesystem:
   - A node is either a file or a directory.
   - A node's contents change over time.
   - When you change a node's contents, it's still the same node; it's
     just been changed.  So a node's identity isn't bound to a specific
     set of contents.
   - If you rename a node, it's still the same node, just under a
     different name.  So a node's identity isn't bound to a particular
     filename.

   A `node revision' refers to a node's contents at a specific point in
   time.  Changing a node's contents always creates a new revision of that
   node.  Once created, a node revision's contents never change.

   When we create a node, its initial contents are the initial revision of
   the node.  As users make changes to the node over time, we create new
   revisions of that same node.  When a user commits a change that deletes
   a file from the filesystem, we don't delete the node, or any revision
   of it --- those stick around to allow us to recreate prior revisions of
   the filesystem.  Instead, we just remove the reference to the node
   from the directory.

   Within the database, we refer to nodes and node revisions using strings
   of numbers separated by periods that look a lot like RCS revision
   numbers.

     node_id ::= number | node_revision_id "." number
     node_revision_id ::= node_id "." number

   So: 
   - "100" is a node id.
   - "100.10" is a node revision id, referring to revision 10 of node 100.
   - "100.10.3" is a node id, referring to the third branch based on
     revision 10 of node 100.
   - "100.10.3.4" is a node revision id, referring to revision 4 of
     of the third branch from revision 10 of node 100.
   And so on.

   Node revision numbers start with 1.  Thus, N.1 is the first revision
   of node N.

   Node / branch numbers start with 1.  Thus, N.M.1 is the first
   branch off of N.M.

   A directory entry identifies the file or subdirectory it refers to
   using a node revision number --- not a node number.  This means that
   a change to a file far down in a directory hierarchy requires the
   parent directory of the changed node to be updated, to hold the new
   node revision ID.  Now, since that parent directory has changed, its
   parent needs to be updated.

   If a particular subtree was unaffected by a given commit, the node
   revision ID that appears in its parent will be unchanged.  When
   doing an update, we can notice this, and ignore that entire
   subtree.  This makes it efficient to find localized changes in
   large trees.

   Note that the number specifying a particular revision of a node is
   unrelated to the global filesystem revision when that node revision
   was created.  So 100.10 may have been created in filesystem revision
   1218; 100.10.3.2 may have been created any time after 100.10; it
   doesn't matter.

   Since revision numbers increase by one each time a delta is added,
   we can compute how many deltas separate two related node revisions
   simply by comparing their ID's.  For example, the distance between
   100.10.3.2 and 100.12 is the distance from 100.10.3.2 to their
   common ancestor, 100.10 (two deltas), plus the distance from 100.10
   to 100.12 (two deltas).

   However, this is kind of a kludge, since the number of deltas is
   not necessarily an accurate indicator of how different two files
   are --- a single delta could be a minor change, or a complete
   replacement.  Furthermore, the filesystem may decide arbitrary to
   store a given node revision as a delta or as full text --- perhaps
   depending on how recently the node was used --- so revision id
   distance isn't necessarily an accurate predictor of retrieval time.

   If you have insights about how this stuff could work better, let me
   know.  I've read some of Josh MacDonald's stuff on this; his
   discussion seems to be mostly about how to retrieve things quickly,
   which is important, but only part of the issue.  I'd like to find
   better ways to recognize renames, and find appropriate ancestors in
   a source tree for changed files.  */


/* Within the code, we represent node and node revision ID's as arrays
   of integers, terminated by a -1 element.  This is the type of an
   element of a node ID.  */
typedef svn_revnum_t svn_fs_id_t;


/* Return the number of components in ID, not including the final -1.  */
int svn_fs_id_length (const svn_fs_id_t *id);


/* Return non-zero iff the node or node revision ID's A and B are equal.  */
int svn_fs_id_eq (const svn_fs_id_t *a, const svn_fs_id_t *b);


/* Return non-zero iff node revision A is an ancestor of node revision B.  
   If A == B, then we consider A to be an ancestor of B.  */
int svn_fs_id_is_ancestor (const svn_fs_id_t *a, const svn_fs_id_t *b);


/* Return the distance between node revisions A and B.  Return -1 if
   they are completely unrelated.  */
int svn_fs_id_distance (const svn_fs_id_t *a, const svn_fs_id_t *b);


/* Return a copy of ID, allocated from POOL.  */
svn_fs_id_t *svn_fs_copy_id (const svn_fs_id_t *id, apr_pool_t *pool);


/* Parse the LEN bytes at DATA as a node or node revision ID.  Return
   zero if the bytes are not a properly-formed ID.  A properly formed
   ID matches the regexp:

       [0-9]+(\.[0-9]+)*

   Allocate the parsed ID in POOL.  If POOL is zero, malloc the ID; we
   need this in certain cases where we can't pass in a pool, but it's
   generally best to use a pool whenever possible.  */
svn_fs_id_t *svn_fs_parse_id (const char *data, apr_size_t len,
                              apr_pool_t *pool);


/* Return a Subversion string containing the unparsed form of the node
   or node revision id ID.  Allocate the string containing the
   unparsed form in POOL.  */
svn_string_t *svn_fs_unparse_id (const svn_fs_id_t *id, apr_pool_t *pool);


/* Nodes.  */

/* An svn_fs_node_t object refers to a node in a filesystem.

   Every node is reached via some path from the root directory of a
   revision, or a transaction.  A node object remembers the revision
   or transaction whose root it was reached from, and the path taken
   to it.

   If a node is reached via the root directory of some transaction T,
   it can be changed.  This will make mutable clones of the node and
   its parents, if they are not mutable already; the new mutable nodes
   will be part of transaction T's tree.  */

typedef struct svn_fs_node_t svn_fs_node_t;


/* Free the node object NODE.  */
void svn_fs_close_node (svn_fs_node_t *node);


/* Return non-zero iff NODE is a...  */
int svn_fs_node_is_dir (svn_fs_node_t *node);
int svn_fs_node_is_file (svn_fs_node_t *node);


/* Return a copy of NODE's ID, allocated in POOL.

   Note that NODE's ID may change over time.  If NODE is an immutable
   node reached via the root directory of some transaction, and
   changes to NODE or its children create a mutable clone of that
   node, then this node object's ID is updated to refer to the mutable
   clone.  */
svn_fs_id_t *svn_fs_get_node_id (svn_fs_node_t *node,
				 apr_pool_t *pool);


/* If NODE was reached via the root of a transaction, return the ID of
   that transaction, as a null-terminated string allocated in POOL.
   Otherwise, return zero.  */
const char *svn_fs_get_node_txn (svn_fs_node_t *node,
				 apr_pool_t *pool);


/* If NODE was reached via the root of a revision, return the number
   of that revision.  Otherwise, return -1.  */
svn_revnum_t svn_fs_get_node_rev (svn_fs_node_t *node);


/* Set *VALUE_P to the value of the property of NODE named PROPNAME.
   If NODE has no property by that name, set *VALUE_P to zero.
   Allocate the result in POOL.  */
svn_error_t *svn_fs_get_node_prop (svn_string_t **value_p,
				   svn_fs_node_t *node,
				   svn_string_t *propname,
				   apr_pool_t *pool);
   

/* Set *TABLE_P to the entire property list of NODE, as an APR hash
   table allocated in POOL.  The resulting table maps property names
   to pointers to svn_string_t objects containing the property value.  */
svn_error_t *svn_fs_get_node_proplist (apr_hash_t **table_p,
				       svn_fs_node_t *node,
				       apr_pool_t *pool);


/* Change a node's property's value, or add/delete a property.
   - NODE is the node whose property should change.  NODE must have
     been reached via the root directory of some transaction, not of a
     revision.
   - NAME is the name of the property to change.
   - VALUE is the new value of the property, or zero if the property should
     be removed altogether.
     
   This creates new mutable clones of any immutable parent directories
   of the node being changed.  If you have any other node objects that
   refer to the cloned directories, that reached them via the same
   transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_change_node_prop (svn_fs_node_t *node,
				      svn_string_t *name,
				      svn_string_t *value,
				      apr_pool_t *pool);


/* Given two nodes SOURCE and TARGET, and a common ancestor ANCESTOR,
   modify TARGET to contain all the changes made between ANCESTOR and
   SOURCE, as well as the changes made between ANCESTOR and TARGET.
   TARGET must have been reached via the root directory of some
   transaction, not of a revision.

   If there are differences between ANCESTOR and SOURCE that conflict
   with changes between ANCESTOR and TARGET, this function returns an
   SVN_ERR_FS_CONFLICT error, and sets *CONFLICT_P to the name of the
   node which couldn't be merged, relative to TARGET.

   This creates new mutable clones of any immutable parent directories
   of TARGET.  If you have any other node objects that refer to the
   cloned directories, that reached them via the same transaction root
   as PARENT, this function updates those node objects to refer to the
   new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_merge (const char **conflict_p,
			   svn_fs_node_t *source,
			   svn_fs_node_t *target,
			   svn_fs_node_t *ancestor,
			   apr_pool_t *pool);



/* Directories.  */


/* Here are the rules for directory entry names, and directory paths:

   A directory entry name is a Unicode string encoded in UTF-8, and
   may not contain the null character (U+0000).  The name should be in
   Unicode canonical decomposition and ordering.  No directory entry
   may be named '.' or '..'.  Given a directory entry name which fails
   to meet these requirements, a filesystem function returns an
   SVN_ERR_FS_PATH_SYNTAX error.

   A directory path is a sequence of one or more directory entry
   names, separated by slash characters (U+002f).  Sequences of two or
   more consecutive slash characters are treated like a single slash.
   If a path ends with a slash, it refers to the same node it would
   without the slash, but that node must be a directory, or else the
   function returns an SVN_ERR_FS_NOT_DIRECTORY error.

   Paths may not start with a slash.  All directory paths in
   Subversion are relative; all functions that expect a path as an
   argument also expect a directory the path should be interpreted
   relative to.  If a function receives a path that begins with a
   slash, it will return an SVN_ERR_FS_PATH_SYNTAX error.  */


/* Set *CHILD_P to a node object representing the existing node named
   PATH relative to the directory PARENT.

   Allocate the node object in POOL.  The node will be closed when
   POOL is destroyed, if it hasn't already been closed explicitly with
   `svn_fs_close_node'.  */
svn_error_t *svn_fs_open_node (svn_fs_node_t **child_p,
			       svn_fs_node_t *parent,
			       const char *path,
			       apr_pool_t *pool);


/* The type of a Subversion directory entry.  */
typedef struct svn_fs_dirent_t {

  /* The name of this directory entry.  */
  char *name;

  /* The node revision ID it names.  */
  svn_fs_id_t *id;

} svn_fs_dirent_t;


/* Set *TABLE_P to a newly allocated APR hash table containing the
   entries of the directory DIR.  The keys of the table are entry
   names, as byte strings; the table's values are pointers to
   svn_fs_dirent_t structures.  Allocate the table and its contents in
   POOL.  */
svn_error_t *svn_fs_dir_entries (apr_hash_t **table_p,
				 svn_fs_node_t *dir,
				 apr_pool_t *pool);


/* Create a new directory named PATH relative to PARENT.  The new
   directory has no entries, and no properties.  PARENT must have been
   reached via the root directory of some transaction, not of a
   revision.

   This creates new mutable clones of any immutable parent directories
   of the new directory.  If you have any other node objects that
   refer to the cloned directories, that reached them via the same
   transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_make_dir (svn_fs_node_t *parent,
			      char *path,
			      apr_pool_t *pool);
			      

/* Delete the node named PATH relative to directory PARENT.  If the
   node being deleted is a directory, it must be empty.  PARENT must
   have been reached via the root directory of some transaction, not
   of a revision.

   This creates new mutable clones of any immutable parent directories
   of the directory being changed.  If you have any other node objects
   that refer to the cloned directories, that reached them via the
   same transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_delete (svn_fs_node_t *parent,
			    const char *path,
			    apr_pool_t *pool);


/* Delete the node named PATH relative to directory PARENT.  If the
   node being deleted is a directory, its contents will be deleted
   recursively.  PARENT must have been reached via the root directory
   of some transaction, not of a revision.

   This creates new mutable clones of any immutable parent directories
   of the directory being changed.  If you have any other node objects
   that refer to the cloned directories, that reached them via the
   same transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_delete_tree (svn_fs_node_t *parent,
				 const char *path,
				 apr_pool_t *pool);


/* Move the node named OLDPATH relative to OLDPARENT to NEWPATH
   relative to NEWPARENT.  OLDPARENT and NEWPARENT must have been
   reached via the root directory of the same transaction.

   This creates new mutable clones of any immutable parent directories
   of the directories being changed.  If you have any other node objects
   that refer to the cloned directories, that reached them via the
   same transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_rename (svn_fs_node_t *old_parent,
			    const char *old_path,
			    svn_fs_node_t *new_parent,
			    const char *new_path,
			    apr_pool_t *pool);


/* Create a a copy of CHILD named PATH relative to PARENT.  PARENT
   must have been reached via the root directory of some transaction,
   not of a revision.  If CHILD is a directory, this copies the tree it
   refers to recursively.

   At the moment, CHILD must be an immutable node.  (This makes the
   implementation trivial: since CHILD is immutable, there is no
   detectable difference between copying CHILD and simply adding a
   reference to it.  However, there's no reason not to extend this to
   mutable nodes --- it's just more (straightforward) code.)

   This creates new mutable clones of any immutable parent directories
   of the directory being changed.  If you have any other node objects
   that refer to the cloned directories, that reached them via the
   same transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_copy (svn_fs_node_t *parent,
			  const char *path,
			  svn_fs_node_t *child,
			  apr_pool_t *pool);



/* Files.  */


/* Set *LENGTH_P to the length of the file FILE, in bytes.  Do any
   necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_file_length (apr_off_t *length_p,
				 svn_fs_node_t *file,
				 apr_pool_t *pool);


/* Set *CONTENTS to a readable generic stream will yield the contents
   of FILE.  Allocate the stream in POOL.  You can only use *CONTENTS
   for as long as the underlying filesystem is open.  */
svn_error_t *svn_fs_file_contents (svn_stream_t **contents,
				   svn_fs_node_t *file,
				   apr_pool_t *pool);


/* Free the file content baton BATON.  */
void svn_fs_free_file_contents (void *baton);


/* Create a new file named PATH relative to PARENT.  The file's
   initial contents are the empty string, and it has no properties.
   PARENT must have been reached via the root directory of some
   transaction, not of a revision.

   This creates new mutable clones of any immutable parent directories
   of the new file.  If you have any other node objects that refer to
   the cloned directories, that reached them via the same transaction
   root as PARENT, this function updates those node objects to refer
   to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_make_file (svn_fs_node_t *parent,
			       const char *path,
			       apr_pool_t *pool);


/* Apply a text delta to the file FILE.  FILE must have been reached
   via the root directory of some transaction, not of a revision.

   Set *CONTENTS_P to a function ready to receive text delta windows
   describing how to change the file's contents, relative to its
   current contents.  Set *CONTENTS_BATON_P to a baton to pass to
   *CONTENTS_P.

   This creates new mutable clones of any immutable parent directories
   of the file being changed.  If you have any other node objects
   that refer to the cloned directories, that reached them via the
   same transaction root as PARENT, this function updates those node
   objects to refer to the new clones.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_apply_textdelta (svn_txdelta_window_handler_t **contents_p,
				     void **contents_baton_p,
				     svn_fs_node_t *file,
				     apr_pool_t *pool);



/* Transactions.  */


/* To make a change to a Subversion filesystem:
   - Create a transaction object, using `svn_fs_begin_txn'.
   - Call `svn_fs_txn_root', to get the transaction's root directory.
   - Make whatever changes you like in that tree.
   - Commit the transaction, using `svn_fs_commit_txn'.

   The filesystem implementation guarantees that your commit will
   either:
   - succeed completely, so that all of the changes are committed to
     create a new revision of the filesystem, or
   - fail completely, leaving the filesystem unchanged.

   Until you commit the transaction, any changes you make are
   invisible.  Only when your commit succeeds do they become visible
   to the outside world, as a new revision of the filesystem.

   If you begin a transaction, and then decide you don't want to make
   the change after all (say, because your net connection with the
   client disappeared before the change was complete), you can call
   `svn_fs_abort_txn', to cancel the entire transaction; this
   leaves the filesystem unchanged.

   The only way to change the contents of files or directories, or
   their properties, is by making a transaction and creating a new
   revision, as described above.  Once a revision has been committed, it
   never changes again; the filesystem interface provides no means to
   go back and edit the contents of an old revision.  Once history has
   been recorded, it is set in stone.  Clients depend on this property
   to do updates and commits reliably; proxies depend on this property
   to cache changes accurately; and so on.


   There are two kinds of nodes in the filesystem: mutable, and
   immutable.  The committed revisions in the filesystem consist
   entirely of immutable nodes, whose contents never change.  A
   transaction in progress, which the user is still constructing, uses
   mutable nodes for those nodes which have been changed so far, and
   refers to immutable nodes for portions of the tree which haven't
   been changed yet in this transaction.

   Immutable nodes, as part of committed transactions, never refer to
   mutable nodes, which are part of uncommitted transactions.  Mutable
   nodes may refer to immutable nodes, or other mutable nodes.

   Note that the terms "immutable" and "mutable" describe whether the
   nodes are part of a committed filesystem revision or not --- not
   the permissions on the nodes they refer to.  Even if you aren't
   authorized to modify the filesystem's root directory, you might be
   authorized to change some descendant of the root; doing so would
   create a new mutable copy of the root directory.  Mutability refers
   to the role of the node: part of an existing revision, or part of a
   new one.  This is independent of your authorization to make changes
   to a given node.


   Transactions are actually persistent objects, stored in the
   database.  You can open a filesystem, begin a transaction, and
   close the filesystem, and then a separate process could open the
   filesystem, pick up the same transaction, and continue work on it.
   When a transaction is successfully committed, it is removed from
   the database.

   Every transaction is assigned a name.  You can open a transaction
   by name, and resume work on it, or find out the name of an existing
   transaction.  You can also list all the transactions currently
   present in the database.

   Transaction names are guaranteed to contain only letters (upper-
   and lower-case), digits, `-', and `.', from the ASCII character
   set.  */



/* The type of a Subversion transaction object.  */
typedef struct svn_fs_txn_t svn_fs_txn_t;


/* Begin a new transaction on the filesystem FS, based on existing
   revision REV.  Set *TXN_P to a pointer to the new transaction.  The
   new transaction's root directory is a mutable successor to the root
   directory of filesystem revision REV.  When committed, this
   transaction will create a new revision.

   Allocate the new transaction in POOL; when POOL is freed, the new
   transaction will be closed (neither committed nor aborted).  If
   POOL is zero, we use FS's internal pool.  You can also close the
   transaction explicitly, using `svn_fs_close_txn'.  */
svn_error_t *svn_fs_begin_txn (svn_fs_txn_t **txn_p,
			       svn_fs_t *fs,
			       svn_revnum_t rev,
			       apr_pool_t *pool);


/* Commit the transaction TXN.  If the transaction conflicts with
   other changes committed to the repository, return an
   SVN_ERR_FS_CONFLICT error.  Otherwise, create a new filesystem
   revision containing the changes made in TXN, and return zero.

   If the commit succeeds, it frees TXN, and any temporary resources
   it holds.  Any node objects referring to formerly mutable nodes
   that were a part of that transaction become invalid; performing any
   operation on them other than closing them will produce an
   SVN_ERR_FS_DEAD_TRANSACTION error.

   If the commit fails, TXN is still valid; you can make more
   operations to resolve the conflict, or call `svn_fs_abort_txn' to
   abort the transaction.  */
svn_error_t *svn_fs_commit_txn (svn_fs_txn_t *txn);


/* Abort the transaction TXN.  Any changes made in TXN are discarded,
   and the filesystem is left unchanged.

   If the commit succeeds, it frees TXN, and any temporary resources
   it holds.  Any node objects referring to formerly mutable nodes
   that were a part of that transaction become invalid; performing any
   operation on them other than closing them will produce an
   SVN_ERR_FS_DEAD_TRANSACTION error.  */
svn_error_t *svn_fs_abort_txn (svn_fs_txn_t *txn);


/* Close the transaction TXN.  This is neither an abort nor a commit;
   the state of the transaction so far is stored in the filesystem, to
   be resumed later.  */
svn_error_t *svn_fs_close_txn (svn_fs_txn_t *txn);


/* Set *DIR_P to the root directory of transaction TXN.

   Allocate the node object in POOL.  The node will be closed when
   POOL is destroyed, if it hasn't already been closed explicitly with
   `svn_fs_close_node'.  */
svn_error_t *svn_fs_open_txn_root (svn_fs_node_t **dir_p,
				   svn_fs_txn_t *txn,
				   apr_pool_t *pool);


/* Set *NAME_P to the name of the transaction TXN, as a
   null-terminated string.  Allocate the name in POOL.  */
svn_error_t *svn_fs_txn_name (char **name_p,
			      svn_fs_txn_t *txn,
			      apr_pool_t *pool);


/* Open the transaction named NAME in the filesystem FS.  Set *TXN to
   the transaction.

   Allocate the new transaction in POOL; when POOL is freed, the new
   transaction will be closed (neither committed nor aborted).  If
   POOL is zero, we use FS's internal pool.  You can also close the
   transaction explicitly, using `svn_fs_close_txn'.  */
svn_error_t *svn_fs_open_txn (svn_fs_txn_t **txn,
			      svn_fs_t *fs,
			      const char *name,
			      apr_pool_t *pool);


/* Set *NAMES_P to a null-terminated array of pointers to strings,
   containing the names of all the currently active transactions in
   the filesystem FS.  Allocate the array in POOL.  */
svn_error_t *svn_fs_list_transactions (char ***names_p,
				       svn_fs_t *fs,
				       apr_pool_t *pool);


/* Filesystem revisions.  */


/* Set *YOUNGEST_P to the number of the youngest revision in filesystem FS.
   The oldest revision in any filesystem is numbered zero.  */
svn_error_t *svn_fs_youngest_rev (svn_revnum_t *youngest_p);


/* Set *DIR_P to the root directory of revision REV of filesystem FS.

   Allocate the node object in POOL.  The node will be closed when
   POOL is destroyed, if it hasn't already been closed explicitly with
   `svn_fs_close_node'.  */
svn_error_t *svn_fs_open_rev_root (svn_fs_node_t **dir_p,
				   svn_fs_t *fs,
				   svn_revnum_t rev,
				   apr_pool_t *pool);


/* Set *VALUE_P to the value of the property name PROPNAME on revision
   REV in the filesystem FS.  If REV has no property by that name, set
   *VALUE_P to zero.  Allocate the result in POOL.  */
svn_error_t *svn_fs_get_rev_prop (svn_string_t **value_p,
				  svn_fs_t *fs,
				  svn_revnum_t rev,
				  svn_string_t *propname,
				  apr_pool_t *pool);


/* Set *TABLE_P to the entire property list of revision REV in
   filesystem FS, as an APR hash table allocated in POOL.  The
   resulting table maps property names to pointers to svn_string_t
   objects containing the property value.  */
svn_error_t *svn_fs_get_rev_proplist (apr_hash_t **table_p,
				      svn_fs_t *fs,
				      svn_revnum_t rev,
				      apr_pool_t *pool);


/* Change a revision's property's value, or add/delete a property.

   - FS is a filesystem, and REV is the revision in that filesystem
     whose property should change.
   - NAME is the name of the property to change.
   - VALUE is the new value of the property, or zero if the property should
     be removed altogether.

   Note that revision properties are non-historied --- you can change
   them after the revision has been committed.  They are not protected
   via transactions.

   Do any necessary temporary allocation in POOL.  */
svn_error_t *svn_fs_change_rev_prop (svn_fs_t *fs,
				     svn_revnum_t rev,
				     svn_string_t *name,
				     svn_string_t *value,
				     apr_pool_t *pool);



/* Computing deltas.  */

/* Compute the differences between SOURCE_DIR and TARGET_DIR, and make
   calls describing those differences on EDITOR, using the provided
   EDIT_BATON.  SOURCE_DIR and TARGET_DIR must be directories from the
   same filesystem.

   The caller must call editor->close_edit on EDIT_BATON;
   svn_fs_dir_delta does not close the edit itself.

   Do any allocation necessary for the delta computation in POOL.
   This function's maximum memory consumption is at most roughly
   proportional to the greatest depth of TARGET_DIR, not the total
   size of the delta.  */
svn_error_t *svn_fs_dir_delta (svn_fs_node_t *source_dir,
			       svn_fs_node_t *target_dir,
			       svn_delta_edit_fns_t *editor,
			       void *edit_baton,
			       apr_pool_t *pool);


/* Set *STREAM_P to a pointer to a delta stream that will turn the
   contents of SOURCE_FILE into the contents of TARGET_FILE.  If
   SOURCE_FILE is zero, treat it as a file with zero length.

   This function does not compare the two files' properties.

   Allocate *STREAM_P, and do any necessary temporary allocation, in
   POOL.  */
svn_error_t *svn_fs_file_delta (svn_txdelta_stream_t **stream_p,
				svn_fs_node_t *source_file,
				svn_fs_node_t *target_file,
				apr_pool_t *pool);



/*** Making changes to a filesystem, editor-style. ***/

/* Hook function type for commits.  When a filesystem commit happens,
 * one of these should be invoked on the NEW_REVISION that resulted
 * from the commit, and the BATON that was provided with the hook
 * originally.
 *
 * See svn_fs_get_editor for an example user.
 */
typedef svn_error_t *svn_fs_commit_hook_t (svn_revnum_t new_revision,
                                           void *baton);


/* Return an EDITOR and EDIT_BATON to commit changes to BASE_REVISION
 * of FS.  The directory baton returned by (*EDITOR)->begin_edit is
 * for the root of the tree; all edits must start at the top and
 * descend. 
 *
 * Calling (*EDITOR)->close_edit completes the commit.  Before
 * close_edit returns, but after the commit has succeeded, it will
 * invoke HOOK with the new revision number and HOOK_BATON as
 * arguments.  If HOOK returns an error, that error will be returned
 * from close_edit, otherwise close_edit will return successfully
 * (unless it encountered an error before invoking HOOK).
 */
svn_error_t *svn_fs_get_editor (svn_delta_edit_fns_t **editor,
                                void **edit_baton,
                                svn_fs_t *fs,
                                svn_revnum_t base_revision,
                                svn_fs_commit_hook_t hook,
                                void *hook_baton,
                                apr_pool_t *pool);




/* Non-historical properties.  */

/* [[Yes, do tell.]] */

#endif /* SVN_FS_H */

#ifdef __cplusplus
}
#endif /* __cplusplus */


/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
