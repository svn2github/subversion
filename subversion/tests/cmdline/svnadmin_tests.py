#!/usr/bin/env python
#
#  svnadmin_tests.py:  testing the 'svnadmin' tool.
#
#  Subversion is a tool for revision control.
#  See http://subversion.apache.org for more information.
#
# ====================================================================
#    Licensed to the Apache Software Foundation (ASF) under one
#    or more contributor license agreements.  See the NOTICE file
#    distributed with this work for additional information
#    regarding copyright ownership.  The ASF licenses this file
#    to you under the Apache License, Version 2.0 (the
#    "License"); you may not use this file except in compliance
#    with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing,
#    software distributed under the License is distributed on an
#    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#    KIND, either express or implied.  See the License for the
#    specific language governing permissions and limitations
#    under the License.
######################################################################

# General modules
import os
import logging
import re
import shutil
import sys
import threading
import time

logger = logging.getLogger()

# Our testing module
import svntest
from svntest.verify import SVNExpectedStdout, SVNExpectedStderr
from svntest.verify import SVNUnexpectedStderr
from svntest.verify import UnorderedOutput
from svntest.main import SVN_PROP_MERGEINFO

# (abbreviation)
Skip = svntest.testcase.Skip_deco
SkipUnless = svntest.testcase.SkipUnless_deco
XFail = svntest.testcase.XFail_deco
Issues = svntest.testcase.Issues_deco
Issue = svntest.testcase.Issue_deco
Wimp = svntest.testcase.Wimp_deco
Item = svntest.wc.StateItem

def check_hotcopy_bdb(src, dst):
  "Verify that the SRC BDB repository has been correctly copied to DST."
  ### TODO: This function should be extended to verify all hotcopied files,
  ### not just compare the output of 'svnadmin dump'. See check_hotcopy_fsfs().
  exit_code, origout, origerr = svntest.main.run_svnadmin("dump", src,
                                                          '--quiet')
  exit_code, backout, backerr = svntest.main.run_svnadmin("dump", dst,
                                                          '--quiet')
  if origerr or backerr or origout != backout:
    raise svntest.Failure

def check_hotcopy_fsfs_fsx(src, dst):
    # Walk the source and compare all files to the destination
    for src_dirpath, src_dirs, src_files in os.walk(src):
      # Verify that the current directory exists in the destination
      dst_dirpath = src_dirpath.replace(src, dst)
      if not os.path.isdir(dst_dirpath):
        raise svntest.Failure("%s does not exist in hotcopy "
                              "destination" % dst_dirpath)
      # Verify that all dirents in the current directory also exist in source
      for dst_dirent in os.listdir(dst_dirpath):
        # Ignore auto-created empty lock files as they may or may not
        # be present and are neither required by nor do they harm to
        # the destination repository.
        if dst_dirent == 'pack-lock':
          continue
        if dst_dirent == 'write-lock':
          continue

        src_dirent = os.path.join(src_dirpath, dst_dirent)
        if not os.path.exists(src_dirent):
          raise svntest.Failure("%s does not exist in hotcopy "
                                "source" % src_dirent)
      # Compare all files in this directory
      for src_file in src_files:
        # Exclude temporary files
        if src_file == 'rev-prop-atomics.shm':
          continue
        if src_file == 'rev-prop-atomics.mutex':
          continue

        # Ignore auto-created empty lock files as they may or may not
        # be present and are neither required by nor do they harm to
        # the destination repository.
        if src_file == 'pack-lock':
          continue
        if src_file == 'write-lock':
          continue

        src_path = os.path.join(src_dirpath, src_file)
        dst_path = os.path.join(dst_dirpath, src_file)
        if not os.path.isfile(dst_path):
          raise svntest.Failure("%s does not exist in hotcopy "
                                "destination" % dst_path)

        # Special case for rep-cache: It will always differ in a byte-by-byte
        # comparison, so compare db tables instead.
        if src_file == 'rep-cache.db':
          db1 = svntest.sqlite3.connect(src_path)
          db2 = svntest.sqlite3.connect(dst_path)
          rows1 = []
          rows2 = []
          for row in db1.execute("select * from rep_cache order by hash"):
            rows1.append(row)
          for row in db2.execute("select * from rep_cache order by hash"):
            rows2.append(row)
          if len(rows1) != len(rows2):
            raise svntest.Failure("number of rows in rep-cache differs")
          for i in range(len(rows1)):
            if rows1[i] != rows2[i]:
              raise svntest.Failure("rep-cache row %i differs: '%s' vs. '%s'"
                                    % (row, rows1[i]))
          continue

        # Special case for revprop-generation: It will always be zero in
        # the hotcopy destination (i.e. a fresh cache generation)
        if src_file == 'revprop-generation':
          f2 = open(dst_path, 'r')
          revprop_gen = int(f2.read().strip())
          if revprop_gen != 0:
              raise svntest.Failure("Hotcopy destination has non-zero " +
                                    "revprop generation")
          continue

        f1 = open(src_path, 'r')
        f2 = open(dst_path, 'r')
        while True:
          offset = 0
          BUFSIZE = 1024
          buf1 = f1.read(BUFSIZE)
          buf2 = f2.read(BUFSIZE)
          if not buf1 or not buf2:
            if not buf1 and not buf2:
              # both at EOF
              break
            elif buf1:
              raise svntest.Failure("%s differs at offset %i" %
                                    (dst_path, offset))
            elif buf2:
              raise svntest.Failure("%s differs at offset %i" %
                                    (dst_path, offset))
          if len(buf1) != len(buf2):
            raise svntest.Failure("%s differs in length" % dst_path)
          for i in range(len(buf1)):
            if buf1[i] != buf2[i]:
              raise svntest.Failure("%s differs at offset %i"
                                    % (dst_path, offset))
            offset += 1
        f1.close()
        f2.close()

def check_hotcopy_fsfs(src, dst):
    "Verify that the SRC FSFS repository has been correctly copied to DST."
    check_hotcopy_fsfs_fsx(src, dst)

def check_hotcopy_fsx(src, dst):
    "Verify that the SRC FSX repository has been correctly copied to DST."
    check_hotcopy_fsfs_fsx(src, dst)
        
#----------------------------------------------------------------------

# How we currently test 'svnadmin' --
#
#   'svnadmin create':   Create an empty repository, test that the
#                        root node has a proper created-revision,
#                        because there was once a bug where it
#                        didn't.
#
#                        Note also that "svnadmin create" is tested
#                        implicitly every time we run a python test
#                        script.  (An empty repository is always
#                        created and then imported into;  if this
#                        subcommand failed catastrophically, every
#                        test would fail and we would know instantly.)
#
#   'svnadmin createtxn'
#   'svnadmin rmtxn':    See below.
#
#   'svnadmin lstxns':   We don't care about the contents of transactions;
#                        we only care that they exist or not.
#                        Therefore, we can simply parse transaction headers.
#
#   'svnadmin dump':     A couple regression tests that ensure dump doesn't
#                        error out, and one to check that the --quiet option
#                        really does what it's meant to do. The actual
#                        contents of the dump aren't verified at all.
#
#  ### TODO:  someday maybe we could parse the contents of trees too.
#
######################################################################
# Helper routines


def get_txns(repo_dir):
  "Get the txn names using 'svnadmin lstxns'."

  exit_code, output_lines, error_lines = svntest.main.run_svnadmin('lstxns',
                                                                   repo_dir)
  txns = sorted([output_lines.strip(x) for x in output_lines])

  return txns

def write_sharded_format(repo_dir, shards):
  """Rewrite the format of the FSFS or FSX repository REPO_DIR so
  that it would use sharding with SHARDS revisions per shard."""

  format_path = os.path.join(repo_dir, "db", "format")
  contents = open(format_path, 'rb').read()
  processed_lines = []

  for line in contents.split("\n"):
    if line.startswith("layout "):
      processed_lines.append("layout sharded %d" % shards)
    else:
      processed_lines.append(line)

  new_contents = "\n".join(processed_lines)
  os.chmod(format_path, 0666)
  open(format_path, 'wb').write(new_contents)

def load_and_verify_dumpstream(sbox, expected_stdout, expected_stderr,
                               revs, check_props, dump, *varargs):
  """Load the array of lines passed in DUMP into the current tests'
  repository and verify the repository content using the array of
  wc.States passed in REVS.  If CHECK_PROPS is True, check properties
  of each rev's items.  VARARGS are optional arguments passed to the
  'load' command."""

  if isinstance(dump, str):
    dump = [ dump ]

  exit_code, output, errput = svntest.main.run_command_stdin(
    svntest.main.svnadmin_binary, expected_stderr, 0, True, dump,
    'load', '--quiet', sbox.repo_dir, *varargs)

  if expected_stdout:
    if expected_stdout == svntest.verify.AnyOutput:
      if len(output) == 0:
        raise SVNExpectedStdout
    else:
      svntest.verify.compare_and_display_lines(
        "Standard output", "STDOUT:", expected_stdout, output)

  if expected_stderr:
    if expected_stderr == svntest.verify.AnyOutput:
      if len(errput) == 0:
        raise SVNExpectedStderr
    else:
      svntest.verify.compare_and_display_lines(
        "Standard error output", "STDERR:", expected_stderr, errput)
    # The expected error occurred, so don't try to verify the result
    return

  if revs:
    # verify revs as wc states
    for rev in range(len(revs)):
      svntest.actions.run_and_verify_svn("Updating to r%s" % (rev+1),
                                         svntest.verify.AnyOutput, [],
                                         "update", "-r%s" % (rev+1),
                                         sbox.wc_dir)

      rev_tree = revs[rev]
      svntest.actions.verify_disk(sbox.wc_dir, rev_tree, check_props)

def load_dumpstream(sbox, dump, *varargs):
  "Load dump text without verification."
  return load_and_verify_dumpstream(sbox, None, None, None, False, dump,
                                    *varargs)

class FSFS_Index:
  """Manages indexes of a rev file in a FSFS format 7 repository.
  The interface returns P2L information and allows for item offsets
  and lengths to be modified. """

  def __init__(self, sbox, revision):
    self.by_item = { }
    self.revision = revision
    self.repo_dir = sbox.repo_dir

    self._read()

  def _read(self):
    """ Read P2L index using svnfsfs. """
    exit_code, output, errput = svntest.main.run_svnfsfs('dump-index',
                                                  '-r' + str(self.revision),
                                                  self.repo_dir)
    svntest.verify.verify_outputs("Error while dumping index",
                                  [], errput, [], [])
    svntest.verify.verify_exit_code(None, exit_code, 0)

    self.by_item.clear()
    for line in output:
      values = line.split()
      if len(values) >= 4 and values[0] != 'Start':
        item = long(values[4])
        self.by_item[item] = values

  def _write(self):
    """ Rewrite indexes using svnfsfs. """
    by_offset = {}
    for values in self.by_item.itervalues():
      by_offset[long(values[0], 16)] = values

    lines = []
    for (offset, values) in sorted(by_offset.items()):
      values = by_offset[offset]
      line = values[0] + ' ' + values[1] + ' ' + values[2] + ' ' + \
             values[3] + ' ' + values[4] + '\n';
      lines.append(line)

    exit_code, output, errput = svntest.main.run_command_stdin(
      svntest.main.svnfsfs_binary, 0, 0, True, lines,
      'load-index', self.repo_dir)

    svntest.verify.verify_outputs("Error while rewriting index",
                                  output, errput, [], [])
    svntest.verify.verify_exit_code(None, exit_code, 0)

  def get_item(self, item):
    """ Return offset, length and type of ITEM. """
    values = self.by_item[item]

    offset = long(values[0], 16)
    len = long(values[1], 16)
    type = values[2]

    return (offset, len, type)

  def modify_item(self, item, offset, len):
    """ Modify offset and length of ITEM. """
    values = self.by_item[item]

    values[0] = '%x' % offset
    values[1] = '%x' % len

    self._write()

def repo_format(sbox):
  """ Return the repository format number for SBOX."""

  format_file = open(os.path.join(sbox.repo_dir, "db", "format"))
  format = int(format_file.read()[:1])
  format_file.close()

  return format

def set_changed_path_list(sbox, revision, changes):
  """ Replace the changed paths list in the revision file REVISION in SBOX
      with the text CHANGES."""

  idx = None

  # read full file
  fp = open(fsfs_file(sbox.repo_dir, 'revs', str(revision)), 'r+b')
  contents = fp.read()
  length = len(contents)

  if repo_format(sbox) < 7:
    # replace the changed paths list
    header = contents[contents.rfind('\n', length - 64, length - 1):]
    body_len = long(header.split(' ')[1])

  else:
    # read & parse revision file footer
    footer_length = ord(contents[length-1]);
    footer = contents[length - footer_length - 1:length-1]
    l2p_offset = long(footer.split(' ')[0])
    p2l_offset = long(footer.split(' ')[1])

    idx = FSFS_Index(sbox, revision)
    (offset, item_len, item_type) = idx.get_item(1)

    # split file contents
    body_len = offset
    indexes = contents[l2p_offset:length - footer_length - 1]

    # construct new footer, include indexes as are
    file_len = body_len + len(changes) + 1
    p2l_offset += file_len - l2p_offset

    header = str(file_len) + ' ' + str(p2l_offset)
    header += chr(len(header))
    header = '\n' + indexes + header

  contents = contents[:body_len] + changes + header

  # set new contents
  fp.seek(0)
  fp.write(contents)
  fp.truncate()
  fp.close()

  if repo_format(sbox) >= 7:
    idx.modify_item(1, offset, len(changes) + 1)

######################################################################
# Tests


#----------------------------------------------------------------------

def test_create(sbox, minor_version=None):
  "'svnadmin create'"


  repo_dir = sbox.repo_dir
  wc_dir = sbox.wc_dir

  svntest.main.safe_rmtree(repo_dir, 1)
  svntest.main.safe_rmtree(wc_dir)

  svntest.main.create_repos(repo_dir, minor_version)

  svntest.actions.run_and_verify_svn("Creating rev 0 checkout",
                                     ["Checked out revision 0.\n"], [],
                                     "checkout",
                                     sbox.repo_url, wc_dir)


  svntest.actions.run_and_verify_svn(
    "Running status",
    [], [],
    "status", wc_dir)

  svntest.actions.run_and_verify_svn(
    "Running verbose status",
    ["                 0        0  ?           %s\n" % wc_dir], [],
    "status", "--verbose", wc_dir)

  # success


# dump stream tests need a dump file

def clean_dumpfile():
  return \
  [ "SVN-fs-dump-format-version: 2\n\n",
    "UUID: 668cc64a-31ed-0310-8ccb-b75d75bb44e3\n\n",
    "Revision-number: 0\n",
    "Prop-content-length: 56\n",
    "Content-length: 56\n\n",
    "K 8\nsvn:date\nV 27\n2005-01-08T21:48:13.838745Z\nPROPS-END\n\n\n",
    "Revision-number: 1\n",
    "Prop-content-length: 98\n",
    "Content-length: 98\n\n",
    "K 7\nsvn:log\nV 0\n\nK 10\nsvn:author\nV 4\nerik\n",
    "K 8\nsvn:date\nV 27\n2005-01-08T21:51:16.313791Z\nPROPS-END\n\n\n",
    "Node-path: A\n",
    "Node-kind: file\n",
    "Node-action: add\n",
    "Prop-content-length: 35\n",
    "Text-content-length: 5\n",
    "Text-content-md5: e1cbb0c3879af8347246f12c559a86b5\n",
    "Content-length: 40\n\n",
    "K 12\nsvn:keywords\nV 2\nId\nPROPS-END\ntext\n\n\n"]

dumpfile_revisions = \
  [ svntest.wc.State('', { 'A' : svntest.wc.StateItem(contents="text\n") }) ]

#----------------------------------------------------------------------
def extra_headers(sbox):
  "loading of dumpstream with extra headers"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  dumpfile[3:3] = \
       [ "X-Comment-Header: Ignored header normally not in dump stream\n" ]

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, False, dumpfile,
                             '--ignore-uuid')

#----------------------------------------------------------------------
# Ensure loading continues after skipping a bit of unknown extra content.
def extra_blockcontent(sbox):
  "load success on oversized Content-length"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  # Replace "Content-length" line with two lines
  dumpfile[8:9] = \
       [ "Extra-content-length: 10\n",
         "Content-length: 108\n\n" ]
  # Insert the extra content after "PROPS-END\n"
  dumpfile[11] = dumpfile[11][:-2] + "extra text\n\n\n"

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, False, dumpfile,
                             '--ignore-uuid')

#----------------------------------------------------------------------
def inconsistent_headers(sbox):
  "load failure on undersized Content-length"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  dumpfile[-2] = "Content-length: 30\n\n"

  load_and_verify_dumpstream(sbox, [], svntest.verify.AnyOutput,
                             dumpfile_revisions, False, dumpfile)

#----------------------------------------------------------------------
# Test for issue #2729: Datestamp-less revisions in dump streams do
# not remain so after load
@Issue(2729)
def empty_date(sbox):
  "preserve date-less revisions in load"

  test_create(sbox)

  dumpfile = clean_dumpfile()

  # Replace portions of the revision data to drop the svn:date revprop.
  dumpfile[7:11] = \
       [ "Prop-content-length: 52\n",
         "Content-length: 52\n\n",
         "K 7\nsvn:log\nV 0\n\nK 10\nsvn:author\nV 4\nerik\nPROPS-END\n\n\n"
         ]

  load_and_verify_dumpstream(sbox,[],[], dumpfile_revisions, False, dumpfile,
                             '--ignore-uuid')

  # Verify that the revision still lacks the svn:date property.
  svntest.actions.run_and_verify_svn(None, [], '.*(E195011|E200017).*svn:date',
                                     "propget", "--revprop", "-r1", "svn:date",
                                     sbox.wc_dir)

#----------------------------------------------------------------------

def dump_copied_dir(sbox):
  "'svnadmin dump' on copied directory"

  sbox.build()
  wc_dir = sbox.wc_dir
  repo_dir = sbox.repo_dir

  old_C_path = os.path.join(wc_dir, 'A', 'C')
  new_C_path = os.path.join(wc_dir, 'A', 'B', 'C')
  svntest.main.run_svn(None, 'cp', old_C_path, new_C_path)
  sbox.simple_commit(message='log msg')

  exit_code, output, errput = svntest.main.run_svnadmin("dump", repo_dir)
  if svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput):
    raise svntest.Failure

#----------------------------------------------------------------------

def dump_move_dir_modify_child(sbox):
  "'svnadmin dump' on modified child of copied dir"

  sbox.build()
  wc_dir = sbox.wc_dir
  repo_dir = sbox.repo_dir

  B_path = os.path.join(wc_dir, 'A', 'B')
  Q_path = os.path.join(wc_dir, 'A', 'Q')
  svntest.main.run_svn(None, 'cp', B_path, Q_path)
  svntest.main.file_append(os.path.join(Q_path, 'lambda'), 'hello')
  sbox.simple_commit(message='log msg')
  exit_code, output, errput = svntest.main.run_svnadmin("dump", repo_dir)
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput)

  exit_code, output, errput = svntest.main.run_svnadmin("dump", "-r",
                                                        "0:HEAD", repo_dir)
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', ["* Dumped revision 0.\n",
               "* Dumped revision 1.\n",
               "* Dumped revision 2.\n"], errput)

#----------------------------------------------------------------------

def dump_quiet(sbox):
  "'svnadmin dump --quiet'"

  sbox.build(create_wc = False)

  exit_code, output, errput = svntest.main.run_svnadmin("dump", sbox.repo_dir,
                                                        '--quiet')
  svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump --quiet' is unexpected.",
    'STDERR', [], errput)

#----------------------------------------------------------------------

def hotcopy_dot(sbox):
  "'svnadmin hotcopy PATH .'"
  sbox.build()

  backup_dir, backup_url = sbox.add_repo_path('backup')
  os.mkdir(backup_dir)
  cwd = os.getcwd()

  os.chdir(backup_dir)
  svntest.actions.run_and_verify_svnadmin(
    None, None, [],
    "hotcopy", os.path.join(cwd, sbox.repo_dir), '.')

  os.chdir(cwd)

  if svntest.main.is_fs_type_fsfs():
    check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  if svntest.main.is_fs_type_bdb():
    check_hotcopy_bdb(sbox.repo_dir, backup_dir)
  if svntest.main.is_fs_type_fsx():
    check_hotcopy_fsx(sbox.repo_dir, backup_dir)

#----------------------------------------------------------------------

# This test is redundant for FSFS. The hotcopy_dot and hotcopy_incremental
# tests cover this check for FSFS already.
@SkipUnless(svntest.main.is_fs_type_bdb)
def hotcopy_format(sbox):
  "'svnadmin hotcopy' checking db/format file"
  sbox.build()

  backup_dir, backup_url = sbox.add_repo_path('backup')
  exit_code, output, errput = svntest.main.run_svnadmin("hotcopy",
                                                        sbox.repo_dir,
                                                        backup_dir)
  if errput:
    logger.warn("Error: hotcopy failed")
    raise svntest.Failure

  # verify that the db/format files are the same
  fp = open(os.path.join(sbox.repo_dir, "db", "format"))
  contents1 = fp.read()
  fp.close()

  fp2 = open(os.path.join(backup_dir, "db", "format"))
  contents2 = fp2.read()
  fp2.close()

  if contents1 != contents2:
    logger.warn("Error: db/format file contents do not match after hotcopy")
    raise svntest.Failure

#----------------------------------------------------------------------

def setrevprop(sbox):
  "setlog, setrevprop, delrevprop; bypass hooks"
  sbox.build()

  # Try a simple log property modification.
  iota_path = os.path.join(sbox.wc_dir, "iota")
  mu_path = sbox.ospath('A/mu')
  svntest.actions.run_and_verify_svnadmin(None, [], [],
                                          "setlog", sbox.repo_dir, "-r0",
                                          "--bypass-hooks",
                                          iota_path)

  # Make sure it fails without --bypass-hooks.  (We haven't called
  # svntest.actions.enable_revprop_changes().)
  #
  # Note that we attempt to set the log message to a different value than the
  # successful call.
  svntest.actions.run_and_verify_svnadmin(None, [], svntest.verify.AnyOutput,
                                          "setlog", sbox.repo_dir, "-r0",
                                          mu_path)

  # Verify that the revprop value matches what we set when retrieved
  # through the client.
  svntest.actions.run_and_verify_svn(None,
                                     [ "This is the file 'iota'.\n", "\n" ],
                                     [], "propget", "--revprop", "-r0",
                                     "svn:log", sbox.wc_dir)

  # Try an author property modification.
  foo_path = os.path.join(sbox.wc_dir, "foo")
  svntest.main.file_write(foo_path, "foo")

  exit_code, output, errput = svntest.main.run_svnadmin("setrevprop",
                                                        sbox.repo_dir,
                                                        "-r0", "svn:author",
                                                        foo_path)
  if errput:
    logger.warn("Error: 'setrevprop' failed")
    raise svntest.Failure

  # Verify that the revprop value matches what we set when retrieved
  # through the client.
  svntest.actions.run_and_verify_svn(None, [ "foo\n" ], [], "propget",
                                     "--revprop", "-r0", "svn:author",
                                     sbox.wc_dir)

  # Delete the property.
  svntest.actions.run_and_verify_svnadmin(None, [], [],
                                          "delrevprop", "-r0", sbox.repo_dir,
                                          "svn:author")
  svntest.actions.run_and_verify_svnlook(None, [], ".*E200017.*svn:author.*",
                                         "propget", "--revprop", "-r0",
                                         sbox.repo_dir, "svn:author")

def verify_windows_paths_in_repos(sbox):
  "verify a repository containing paths like 'c:hi'"

  # setup a repo with a directory 'c:hi'
  sbox.build(create_wc = False)
  repo_url       = sbox.repo_url
  chi_url = sbox.repo_url + '/c:hi'

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir', '-m', 'log_msg',
                                     chi_url)

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  # unfortunately, some backends needs to do more checks than other
  # resulting in different progress output
  if svntest.main.is_fs_log_addressing():
    svntest.verify.compare_and_display_lines(
      "Error while running 'svnadmin verify'.",
      'STDOUT', ["* Verifying metadata at revision 0 ...\n",
                 "* Verifying repository metadata ...\n",
                 "* Verified revision 0.\n",
                 "* Verified revision 1.\n",
                 "* Verified revision 2.\n"], output)
  elif svntest.main.fs_has_rep_sharing():
    svntest.verify.compare_and_display_lines(
      "Error while running 'svnadmin verify'.",
      'STDOUT', ["* Verifying repository metadata ...\n",
                 "* Verified revision 0.\n",
                 "* Verified revision 1.\n",
                 "* Verified revision 2.\n"], output)
  else:
    svntest.verify.compare_and_display_lines(
      "Error while running 'svnadmin verify'.",
      'STDOUT', ["* Verified revision 0.\n",
                 "* Verified revision 1.\n",
                 "* Verified revision 2.\n"], output)

#----------------------------------------------------------------------

# Returns the filename of the rev or revprop file (according to KIND)
# numbered REV in REPO_DIR, which must be in the first shard if we're
# using a sharded repository.
def fsfs_file(repo_dir, kind, rev):
  if svntest.main.options.server_minor_version >= 5:
    if svntest.main.options.fsfs_sharding is None:
      return os.path.join(repo_dir, 'db', kind, '0', rev)
    else:
      shard = int(rev) // svntest.main.options.fsfs_sharding
      path = os.path.join(repo_dir, 'db', kind, str(shard), rev)

      if svntest.main.options.fsfs_packing is None or kind == 'revprops':
        # we don't pack revprops
        return path
      elif os.path.exists(path):
        # rev exists outside a pack file.
        return path
      else:
        # didn't find the plain file; assume it's in a pack file
        return os.path.join(repo_dir, 'db', kind, ('%d.pack' % shard), 'pack')
  else:
    return os.path.join(repo_dir, 'db', kind, rev)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def verify_incremental_fsfs(sbox):
  """svnadmin verify detects corruption dump can't"""

  # setup a repo with a directory 'c:hi'
  # use physical addressing as this is hard to provoke with logical addressing
  sbox.build(create_wc = False,
             minor_version = min(svntest.main.options.server_minor_version,8))
  repo_url = sbox.repo_url
  E_url = sbox.repo_url + '/A/B/E'

  # Create A/B/E/bravo in r2.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir', '-m', 'log_msg',
                                     E_url + '/bravo')
  # Corrupt r2's reference to A/C by replacing "dir 7-1.0.r1/1568" with
  # "dir 7-1.0.r1/1569" (increment offset) and updating the checksum for
  # this directory listing to "c9b5a2d26473a4e28088673dda9df804" so that
  # the listing itself is valid.
  r2 = fsfs_file(sbox.repo_dir, 'revs', '2')
  if r2.endswith('pack'):
    raise svntest.Skip

  fp = open(r2, 'wb')
  fp.write("""id: 0-2.0.r2/0
type: dir
count: 0
cpath: /A/B/E/bravo
copyroot: 0 /

PLAIN
K 5
alpha
V 17
file 3-1.0.r1/719
K 4
beta
V 17
file 4-1.0.r1/840
K 5
bravo
V 14
dir 0-2.0.r2/0
END
ENDREP
id: 2-1.0.r2/181
type: dir
pred: 2-1.0.r1/1043
count: 1
text: 2 69 99 99 f63001f7fddd1842d8891474d0982111
cpath: /A/B/E
copyroot: 0 /

PLAIN
K 1
E
V 16
dir 2-1.0.r2/181
K 1
F
V 17
dir 5-1.0.r1/1160
K 6
lambda
V 17
file 6-1.0.r1/597
END
ENDREP
id: 1-1.0.r2/424
type: dir
pred: 1-1.0.r1/1335
count: 1
text: 2 316 95 95 bccb66379b4f825dac12b50d80211bae
cpath: /A/B
copyroot: 0 /

PLAIN
K 1
B
V 16
dir 1-1.0.r2/424
K 1
C
V 17
dir 7-1.0.r1/1569
K 1
D
V 17
dir 8-1.0.r1/3061
K 2
mu
V 18
file i-1.0.r1/1451
END
ENDREP
id: 0-1.0.r2/692
type: dir
pred: 0-1.0.r1/3312
count: 1
text: 2 558 121 121 c9b5a2d26473a4e28088673dda9df804
cpath: /A
copyroot: 0 /

PLAIN
K 1
A
V 16
dir 0-1.0.r2/692
K 4
iota
V 18
file j-1.0.r1/3428
END
ENDREP
id: 0.0.r2/904
type: dir
pred: 0.0.r1/3624
count: 2
text: 2 826 65 65 e44e4151d0d124533338619f082c8c9a
cpath: /
copyroot: 0 /

_0.0.t1-1 add false false /A/B/E/bravo


904 1031
""")
  fp.close()

  exit_code, output, errput = svntest.main.run_svnadmin("verify", "-r2",
                                                        sbox.repo_dir)
  svntest.verify.verify_outputs(
    message=None, actual_stdout=output, actual_stderr=errput,
    expected_stdout=None,
    expected_stderr=".*Found malformed header '[^']*' in revision file"
                    "|.*Missing id field in node-rev.*")

#----------------------------------------------------------------------

# Helper for two test functions.
def corrupt_and_recover_db_current(sbox, minor_version=None):
  """Build up a MINOR_VERSION sandbox and test different recovery scenarios
  with missing, out-of-date or even corrupt db/current files.  Recovery should
  behave the same way with all values of MINOR_VERSION, hence this helper
  containing the common code that allows us to check it."""

  sbox.build(minor_version=minor_version)
  current_path = os.path.join(sbox.repo_dir, 'db', 'current')

  # Commit up to r3, so we can test various recovery scenarios.
  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newer line\n')
  sbox.simple_commit(message='log msg')

  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newest line\n')
  sbox.simple_commit(message='log msg')

  # Remember the contents of the db/current file.
  expected_current_contents = open(current_path).read()

  # Move aside the current file for r3.
  os.rename(os.path.join(sbox.repo_dir, 'db','current'),
            os.path.join(sbox.repo_dir, 'db','was_current'))

  # Run 'svnadmin recover' and check that the current file is recreated.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = open(current_path).read()
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

  # Now try writing db/current to be one rev lower than it should be.
  svntest.main.file_write(current_path, '2\n')

  # Run 'svnadmin recover' and check that the current file is fixed.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = open(current_path).read()
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

  # Now try writing db/current to be *two* revs lower than it should be.
  svntest.main.file_write(current_path, '1\n')

  # Run 'svnadmin recover' and check that the current file is fixed.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = open(current_path).read()
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)

  # Now try writing db/current to be fish revs lower than it should be.
  #
  # Note: I'm not actually sure it's wise to recover from this, but
  # detecting it would require rewriting fs_fs.c:get_youngest() to
  # check the actual contents of its buffer, since atol() will happily
  # convert "fish" to 0.
  svntest.main.file_write(current_path, 'fish\n')

  # Run 'svnadmin recover' and check that the current file is fixed.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)

  actual_current_contents = open(current_path).read()
  svntest.verify.compare_and_display_lines(
    "Contents of db/current is unexpected.",
    'db/current', expected_current_contents, actual_current_contents)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_recover_db_current(sbox):
  "fsfs recover db/current"
  corrupt_and_recover_db_current(sbox)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_recover_old_db_current(sbox):
  "fsfs recover db/current --compatible-version=1.3"

  # Around trunk@1573728, 'svnadmin recover' wrongly errored out
  # for the --compatible-version=1.3 repositories with missing or
  # invalid db/current file:
  # svnadmin: E160006: No such revision 1

  corrupt_and_recover_db_current(sbox, minor_version=3)

#----------------------------------------------------------------------
@Issue(2983)
def load_with_parent_dir(sbox):
  "'svnadmin load --parent-dir' reparents mergeinfo"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2983. ##
  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svnadmin_tests_data',
                                   'mergeinfo_included.dump')
  dumpfile = open(dumpfile_location).read()

  # Create 'sample' dir in sbox.repo_url, and load the dump stream there.
  svntest.actions.run_and_verify_svn(None,
                                     ['Committing transaction...\n',
                                      'Committed revision 1.\n'],
                                     [], "mkdir", sbox.repo_url + "/sample",
                                     "-m", "Create sample dir")
  load_dumpstream(sbox, dumpfile, '--parent-dir', '/sample')

  # Verify the svn:mergeinfo properties for '--parent-dir'
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample/branch - /sample/trunk:5-7\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample/branch')
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample/branch1 - " +
                                      "/sample/branch:6-9\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample/branch1')

  # Create 'sample-2' dir in sbox.repo_url, and load the dump stream again.
  # This time, don't include a leading slash on the --parent-dir argument.
  # See issue #3547.
  svntest.actions.run_and_verify_svn(None,
                                     ['Committing transaction...\n',
                                      'Committed revision 11.\n'],
                                     [], "mkdir", sbox.repo_url + "/sample-2",
                                     "-m", "Create sample-2 dir")
  load_dumpstream(sbox, dumpfile, '--parent-dir', 'sample-2')

  # Verify the svn:mergeinfo properties for '--parent-dir'.
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample-2/branch - " +
                                      "/sample-2/trunk:15-17\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample-2/branch')
  svntest.actions.run_and_verify_svn(None,
                                     [sbox.repo_url +
                                      "/sample-2/branch1 - " +
                                      "/sample-2/branch:16-19\n"],
                                     [], 'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url + '/sample-2/branch1')

#----------------------------------------------------------------------

def set_uuid(sbox):
  "test 'svnadmin setuuid'"

  sbox.build(create_wc=False)

  # Squirrel away the original repository UUID.
  exit_code, output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  orig_uuid = output[0].rstrip()

  # Try setting a new, bogus UUID.
  svntest.actions.run_and_verify_svnadmin(None, None, '^.*Malformed UUID.*$',
                                          'setuuid', sbox.repo_dir, 'abcdef')

  # Try generating a brand new UUID.
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', sbox.repo_dir)
  exit_code, output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  new_uuid = output[0].rstrip()
  if new_uuid == orig_uuid:
    logger.warn("Error: new UUID matches the original one")
    raise svntest.Failure

  # Now, try setting the UUID back to the original value.
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', sbox.repo_dir, orig_uuid)
  exit_code, output, errput = svntest.main.run_svnlook('uuid', sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  new_uuid = output[0].rstrip()
  if new_uuid != orig_uuid:
    logger.warn("Error: new UUID doesn't match the original one")
    raise svntest.Failure

#----------------------------------------------------------------------
@Issue(3020)
def reflect_dropped_renumbered_revs(sbox):
  "reflect dropped renumbered revs in svn:mergeinfo"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=3020. ##

  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svndumpfilter_tests_data',
                                   'with_merges.dump')
  dumpfile = open(dumpfile_location).read()

  # Create 'toplevel' dir in sbox.repo_url
  svntest.actions.run_and_verify_svn(None, ['Committing transaction...\n',
                                            'Committed revision 1.\n'],
                                     [], "mkdir", sbox.repo_url + "/toplevel",
                                     "-m", "Create toplevel dir")

  # Load the dump stream in sbox.repo_url
  load_dumpstream(sbox, dumpfile)

  # Load the dump stream in toplevel dir
  load_dumpstream(sbox, dumpfile, '--parent-dir', '/toplevel')

  # Verify the svn:mergeinfo properties
  url = sbox.repo_url
  expected_output = svntest.verify.UnorderedOutput([
    url + "/trunk - /branch1:5-9\n",
    url + "/toplevel/trunk - /toplevel/branch1:14-18\n",
    ])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)

#----------------------------------------------------------------------

@SkipUnless(svntest.main.is_fs_type_fsfs)
@Issue(2992)
def fsfs_recover_handle_missing_revs_or_revprops_file(sbox):
  """fsfs recovery checks missing revs / revprops files"""
  # Set up a repository containing the greek tree.
  sbox.build()

  # Commit up to r3, so we can test various recovery scenarios.
  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newer line\n')
  sbox.simple_commit(message='log msg')

  svntest.main.file_append(os.path.join(sbox.wc_dir, 'iota'), 'newest line\n')
  sbox.simple_commit(message='log msg')

  rev_3 = fsfs_file(sbox.repo_dir, 'revs', '3')
  rev_was_3 = rev_3 + '.was'

  # Move aside the revs file for r3.
  os.rename(rev_3, rev_was_3)

  # Verify 'svnadmin recover' fails when youngest has a revprops
  # file but no revs file.
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.", None, errput, None,
    ".*Expected current rev to be <= %s but found 3"
    # For example, if svntest.main.fsfs_sharding == 2, then rev_3 would
    # be the pack file for r2:r3, and the error message would report "<= 1".
    % (rev_3.endswith('pack') and '[012]' or '2')):
    raise svntest.Failure

  # Restore the r3 revs file, thus repairing the repository.
  os.rename(rev_was_3, rev_3)

  revprop_3 = fsfs_file(sbox.repo_dir, 'revprops', '3')
  revprop_was_3 = revprop_3 + '.was'

  # Move aside the revprops file for r3.
  os.rename(revprop_3, revprop_was_3)

  # Verify 'svnadmin recover' fails when youngest has a revs file
  # but no revprops file (issue #2992).
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.", None, errput, None,
    ".*Revision 3 has a revs file but no revprops file"):
    raise svntest.Failure

  # Restore the r3 revprops file, thus repairing the repository.
  os.rename(revprop_was_3, revprop_3)

  # Change revprops file to a directory for revision 3
  os.rename(revprop_3, revprop_was_3)
  os.mkdir(revprop_3)

  # Verify 'svnadmin recover' fails when youngest has a revs file
  # but revprops file is not a file (another aspect of issue #2992).
  exit_code, output, errput = svntest.main.run_svnadmin("recover",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin recover' is unexpected.", None, errput, None,
    ".*Revision 3 has a non-file where its revprops file should be.*"):
    raise svntest.Failure


#----------------------------------------------------------------------

def create_in_repo_subdir(sbox):
  "'svnadmin create /path/to/repo/subdir'"

  repo_dir = sbox.repo_dir
  wc_dir = sbox.wc_dir

  svntest.main.safe_rmtree(repo_dir, 1)
  svntest.main.safe_rmtree(wc_dir)

  # This should succeed
  svntest.main.create_repos(repo_dir)

  success = False
  try:
    # This should fail
    subdir = os.path.join(repo_dir, 'Z')
    svntest.main.create_repos(subdir)
  except svntest.main.SVNRepositoryCreateFailure:
    success = True
  if not success:
    raise svntest.Failure

  cwd = os.getcwd()
  success = False
  try:
    # This should fail, too
    subdir = os.path.join(repo_dir, 'conf')
    os.chdir(subdir)
    svntest.main.create_repos('Z')
    os.chdir(cwd)
  except svntest.main.SVNRepositoryCreateFailure:
    success = True
    os.chdir(cwd)
  if not success:
    raise svntest.Failure


@SkipUnless(svntest.main.is_fs_type_fsfs)
def verify_with_invalid_revprops(sbox):
  "svnadmin verify detects invalid revprops file"

  repo_dir = sbox.repo_dir

  svntest.main.safe_rmtree(repo_dir, 1)

  # This should succeed
  svntest.main.create_repos(repo_dir)

  # Run a test verify
  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)

  if errput:
    raise SVNUnexpectedStderr(errput)
  if svntest.verify.verify_outputs(
    "Output of 'svnadmin verify' is unexpected.", None, output, None,
    ".*Verified revision 0*"):
    raise svntest.Failure

  # Empty the revprops file
  rp_file = open(os.path.join(repo_dir, 'db', 'revprops', '0', '0'), 'w')

  rp_file.write('')
  rp_file.close()

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs(
    "Output of 'svnadmin verify' is unexpected.", None, errput, None,
    ".*svnadmin: E200002:.*"):
    raise svntest.Failure

#----------------------------------------------------------------------
# Even *more* testing for issue #3020 'Reflect dropped/renumbered
# revisions in svn:mergeinfo data during svnadmin load'
#
# Full or incremental dump-load cycles should result in the same
# mergeinfo in the loaded repository.
#
# Given a repository 'SOURCE-REPOS' with mergeinfo, and a repository
# 'TARGET-REPOS' (which may or may not be empty), either of the following
# methods to move 'SOURCE-REPOS' to 'TARGET-REPOS' should result in
# the same mergeinfo on 'TARGET-REPOS':
#
#   1) Dump -r1:HEAD from 'SOURCE-REPOS' and load it in one shot to
#      'TARGET-REPOS'.
#
#   2) Dump 'SOURCE-REPOS' in a series of incremental dumps and load
#      each of them to 'TARGET-REPOS'.
#
# See http://subversion.tigris.org/issues/show_bug.cgi?id=3020#desc13
@Issue(3020)
def dont_drop_valid_mergeinfo_during_incremental_loads(sbox):
  "don't filter mergeinfo revs from incremental dump"

  # Create an empty repos.
  test_create(sbox)

  # PART 1: Load a full dump to an empty repository.
  #
  # The test repository used here, 'mergeinfo_included_full.dump', is
  # this repos:
  #                       __________________________________________
  #                      |                                         |
  #                      |             ____________________________|_____
  #                      |            |                            |     |
  # trunk---r2---r3-----r5---r6-------r8---r9--------------->      |     |
  #   r1             |        |     |       |                      |     |
  # initial          |        |     |       |______                |     |
  # import         copy       |   copy             |            merge   merge
  #                  |        |     |            merge           (r5)   (r8)
  #                  |        |     |            (r9)              |     |
  #                  |        |     |              |               |     |
  #                  |        |     V              V               |     |
  #                  |        | branches/B2-------r11---r12---->   |     |
  #                  |        |     r7              |____|         |     |
  #                  |        |                        |           |     |
  #                  |      merge                      |___        |     |
  #                  |      (r6)                           |       |     |
  #                  |        |_________________           |       |     |
  #                  |                          |        merge     |     |
  #                  |                          |      (r11-12)    |     |
  #                  |                          |          |       |     |
  #                  V                          V          V       |     |
  #              branches/B1-------------------r10--------r13-->   |     |
  #                  r4                                            |     |
  #                   |                                            V     V
  #                  branches/B1/B/E------------------------------r14---r15->
  #
  #
  # The mergeinfo on this repos@15 is:
  #
  #   Properties on 'branches/B1':
  #     svn:mergeinfo
  #       /branches/B2:11-12
  #       /trunk:6,9
  #   Properties on 'branches/B1/B/E':
  #     svn:mergeinfo
  #       /branches/B2/B/E:11-12
  #       /trunk/B/E:5-6,8-9
  #   Properties on 'branches/B2':
  #     svn:mergeinfo
  #       /trunk:9
  dumpfile_full = open(os.path.join(os.path.dirname(sys.argv[0]),
                                    'svnadmin_tests_data',
                                    'mergeinfo_included_full.dump')).read()
  load_dumpstream(sbox, dumpfile_full, '--ignore-uuid')

  # Check that the mergeinfo is as expected.
  url = sbox.repo_url + '/branches/'
  expected_output = svntest.verify.UnorderedOutput([
    url + "B1 - /branches/B2:11-12\n",
    "/trunk:6,9\n",
    url + "B2 - /trunk:9\n",
    url + "B1/B/E - /branches/B2/B/E:11-12\n",
    "/trunk/B/E:5-6,8-9\n"])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)

  # PART 2: Load a a series of incremental dumps to an empty repository.
  #
  # Incrementally dump the repository into three dump files:
  dump_file_r1_10 = svntest.main.temp_dir + "-r1-10.dump"
  exit_code, output, errput = svntest.main.run_svnadmin(
    'dump', sbox.repo_dir, '-r1:10')
  dump_fp = open(dump_file_r1_10, 'wb')
  dump_fp.writelines(output)
  dump_fp.close()

  dump_file_r11_13 = svntest.main.temp_dir + "-r11-13.dump"
  exit_code, output, errput = svntest.main.run_svnadmin(
    'dump', sbox.repo_dir, '--incremental', '-r11:13')
  dump_fp = open(dump_file_r11_13, 'wb')
  dump_fp.writelines(output)
  dump_fp.close()

  dump_file_r14_15 = svntest.main.temp_dir + "-r14-15.dump"
  exit_code, output, errput = svntest.main.run_svnadmin(
    'dump', sbox.repo_dir, '--incremental', '-r14:15')
  dump_fp = open(dump_file_r14_15, 'wb')
  dump_fp.writelines(output)
  dump_fp.close()

  # Blow away the current repos and create an empty one in its place.
  test_create(sbox)

  # Load the three incremental dump files in sequence.
  load_dumpstream(sbox, open(dump_file_r1_10).read(), '--ignore-uuid')
  load_dumpstream(sbox, open(dump_file_r11_13).read(), '--ignore-uuid')
  load_dumpstream(sbox, open(dump_file_r14_15).read(), '--ignore-uuid')

  # Check the mergeinfo, we use the same expected output as before,
  # as it (duh!) should be exactly the same as when we loaded the
  # repos in one shot.
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)

  # Now repeat the above two scenarios, but with an initially non-empty target
  # repository.  First, try the full dump-load in one shot.
  #
  # PART 3: Load a full dump to an non-empty repository.
  #
  # Reset our sandbox.
  test_create(sbox)

  # Load this skeleton repos into the empty target:
  #
  #   Projects/       (Added r1)
  #     README        (Added r2)
  #     Project-X     (Added r3)
  #     Project-Y     (Added r4)
  #     Project-Z     (Added r5)
  #     docs/         (Added r6)
  #       README      (Added r6)
  dumpfile_skeleton = open(os.path.join(os.path.dirname(sys.argv[0]),
                                        'svnadmin_tests_data',
                                        'skeleton_repos.dump')).read()
  load_dumpstream(sbox, dumpfile_skeleton, '--ignore-uuid')

  # Load 'svnadmin_tests_data/mergeinfo_included_full.dump' in one shot:
  load_dumpstream(sbox, dumpfile_full, '--parent-dir', 'Projects/Project-X',
                  '--ignore-uuid')

  # Check that the mergeinfo is as expected.  This is exactly the
  # same expected mergeinfo we previously checked, except that the
  # revisions are all offset +6 to reflect the revions already in
  # the skeleton target before we began loading and the leading source
  # paths are adjusted by the --parent-dir:
  #
  #   Properties on 'branches/B1':
  #     svn:mergeinfo
  #       /Projects/Project-X/branches/B2:17-18
  #       /Projects/Project-X/trunk:12,15
  #   Properties on 'branches/B1/B/E':
  #     svn:mergeinfo
  #       /Projects/Project-X/branches/B2/B/E:17-18
  #       /Projects/Project-X/trunk/B/E:11-12,14-15
  #   Properties on 'branches/B2':
  #     svn:mergeinfo
  #       /Projects/Project-X/trunk:15
  url = sbox.repo_url + '/Projects/Project-X/branches/'
  expected_output = svntest.verify.UnorderedOutput([
    url + "B1 - /Projects/Project-X/branches/B2:17-18\n",
    "/Projects/Project-X/trunk:12,15\n",
    url + "B2 - /Projects/Project-X/trunk:15\n",
    url + "B1/B/E - /Projects/Project-X/branches/B2/B/E:17-18\n",
    "/Projects/Project-X/trunk/B/E:11-12,14-15\n"])
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)

  # PART 4: Load a a series of incremental dumps to an non-empty repository.
  #
  # Reset our sandbox.
  test_create(sbox)

  # Load this skeleton repos into the empty target:
  load_dumpstream(sbox, dumpfile_skeleton, '--ignore-uuid')

  # Load the three incremental dump files in sequence.
  load_dumpstream(sbox, open(dump_file_r1_10).read(),
                  '--parent-dir', 'Projects/Project-X', '--ignore-uuid')
  load_dumpstream(sbox, open(dump_file_r11_13).read(),
                  '--parent-dir', 'Projects/Project-X', '--ignore-uuid')
  load_dumpstream(sbox, open(dump_file_r14_15).read(),
                  '--parent-dir', 'Projects/Project-X', '--ignore-uuid')

  # Check the resulting mergeinfo.  We expect the exact same results
  # as Part 3.
  # See http://subversion.tigris.org/issues/show_bug.cgi?id=3020#desc16.
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'propget', 'svn:mergeinfo', '-R',
                                     sbox.repo_url)


@SkipUnless(svntest.main.is_posix_os)
@Issue(2591)
def hotcopy_symlink(sbox):
  "'svnadmin hotcopy' replicates symlink"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=2591. ##

  original_repo = sbox.repo_dir

  hotcopy_repo, hotcopy_url = sbox.add_repo_path('hotcopy')

  # Create a repository.
  svntest.main.safe_rmtree(original_repo, 1)
  svntest.main.create_repos(original_repo)

  # Create a file, a dir and a missing path outside the repoitory.
  svntest.main.safe_rmtree(sbox.wc_dir, 1)
  os.mkdir(sbox.wc_dir)
  external_file_path = os.path.join(sbox.wc_dir, "file")
  svntest.main.file_write(external_file_path, "An existing file")
  external_dir_path = os.path.join(sbox.wc_dir, "dir")
  os.mkdir(external_dir_path)
  external_missing_path = os.path.join(sbox.wc_dir, "missing")

  # Symlink definitions: base name -> target relpath.
  # Check both existing and nonexistent targets.
  # Check targets both within and outside the source repository.
  symlinks = [
    ('in_repos_file',    'format'),
    ('in_repos_dir',     'conf'),
    ('in_repos_missing', 'missing'),
    ('external_file',    os.path.join('..', '..', '..', external_file_path)),
    ('external_dir',     os.path.join('..', '..', '..', external_dir_path)),
    ('external_missing', os.path.join('..', '..', '..', external_missing_path)),
  ]

  # Create symlinks within the repository directory.
  for name, target_relpath in symlinks:
    target_path = os.path.join(original_repo, target_relpath)
    target_abspath = os.path.abspath(target_path)

    # Create two symlinks to each target - one relative, one absolute.
    symlink_path = os.path.join(original_repo, name)
    os.symlink(target_relpath, symlink_path + '_rel')
    os.symlink(target_abspath, symlink_path + '_abs')

  svntest.actions.run_and_verify_svnadmin(
    None, None, [],
    "hotcopy", original_repo, hotcopy_repo)

  # Check if the symlinks were copied correctly.
  for name, target_relpath in symlinks:
    target_path = os.path.join(original_repo, target_relpath)
    target_abspath = os.path.abspath(target_path)

    # Check two symlinks to each target - one relative, one absolute.
    symlink_path = os.path.join(hotcopy_repo, name)
    if os.readlink(symlink_path + '_rel') != target_relpath:
      raise svntest.Failure
    if os.readlink(symlink_path + '_abs') != target_abspath:
      raise svntest.Failure

def load_bad_props(sbox):
  "svnadmin load with invalid svn: props"

  dump_str = """SVN-fs-dump-format-version: 2

UUID: dc40867b-38f6-0310-9f5f-f81aa277e06f

Revision-number: 0
Prop-content-length: 56
Content-length: 56

K 8
svn:date
V 27
2005-05-03T19:09:41.129900Z
PROPS-END

Revision-number: 1
Prop-content-length: 99
Content-length: 99

K 7
svn:log
V 3
\n\r\n
K 10
svn:author
V 2
pl
K 8
svn:date
V 27
2005-05-03T19:10:19.975578Z
PROPS-END

Node-path: file
Node-kind: file
Node-action: add
Prop-content-length: 10
Text-content-length: 5
Text-content-md5: e1cbb0c3879af8347246f12c559a86b5
Content-length: 15

PROPS-END
text


"""
  test_create(sbox)

  # Try to load the dumpstream, expecting a failure (because of mixed EOLs).
  load_and_verify_dumpstream(sbox, [], svntest.verify.AnyOutput,
                             dumpfile_revisions, False, dump_str,
                             '--ignore-uuid')

  # Now try it again bypassing prop validation.  (This interface takes
  # care of the removal and recreation of the original repository.)
  svntest.actions.load_repo(sbox, dump_str=dump_str,
                            bypass_prop_validation=True)

# This test intentionally corrupts a revision and assumes an FSFS
# repository. If you can make it work with BDB please do so.
# However, the verification triggered by this test is in the repos layer
# so it will trigger with either backend anyway.
@SkipUnless(svntest.main.is_fs_type_fsfs)
@SkipUnless(svntest.main.server_enforces_UTF8_fspaths_in_verify)
def verify_non_utf8_paths(sbox):
  "svnadmin verify with non-UTF-8 paths"

  dumpfile = clean_dumpfile()

  # Corruption only possible in physically addressed revisions created
  # with pre-1.6 servers.
  test_create(sbox,
              minor_version = min(svntest.main.options.server_minor_version,8))

  # Load the dumpstream
  load_and_verify_dumpstream(sbox, [], [], dumpfile_revisions, False,
                             dumpfile, '--ignore-uuid')

  # Replace the path 'A' in revision 1 with a non-UTF-8 sequence.
  # This has been observed in repositories in the wild, though Subversion
  # 1.6 and greater should prevent such filenames from entering the repository.
  path1 = os.path.join(sbox.repo_dir, "db", "revs", "0", "1")
  path_new = os.path.join(sbox.repo_dir, "db", "revs", "0", "1.new")
  fp1 = open(path1, 'rb')
  fp_new = open(path_new, 'wb')
  for line in fp1.readlines():
    if line == "A\n":
      # replace 'A' with a latin1 character -- the new path is not valid UTF-8
      fp_new.write("\xE6\n")
    elif line == "text: 1 279 32 32 d63ecce65d8c428b86f4f8b0920921fe\n":
      # phys, PLAIN directories: fix up the representation checksum
      fp_new.write("text: 1 279 32 32 b50b1d5ed64075b5f632f3b8c30cd6b2\n")
    elif line == "text: 1 292 44 32 a6be7b4cf075fd39e6a99eb69a31232b\n":
      # phys, deltified directories: fix up the representation checksum
      fp_new.write("text: 1 292 44 32 f2e93e73272cac0f18fccf16f224eb93\n")
    elif line == "text: 1 6 31 31 90f306aa9bfd72f456072076a2bd94f7\n":
      # log addressing: fix up the representation checksum
      fp_new.write("text: 1 6 31 31 db2d4a0bad5dff0aea9a288dec02f1fb\n")
    elif line == "cpath: /A\n":
      # also fix up the 'created path' field
      fp_new.write("cpath: /\xE6\n")
    elif line == "_0.0.t0-0 add-file true true /A\n":
      # and another occurrance
      fp_new.write("_0.0.t0-0 add-file true true /\xE6\n")
    else:
      fp_new.write(line)
  fp1.close()
  fp_new.close()
  os.remove(path1)
  os.rename(path_new, path1)

  # Verify the repository, expecting failure
  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)
  svntest.verify.verify_outputs(
    "Unexpected error while running 'svnadmin verify'.",
    [], errput, None, ".*Path '.*' is not in UTF-8.*")

  # Make sure the repository can still be dumped so that the
  # encoding problem can be fixed in a dump/edit/load cycle.
  expected_stderr = [
    "* Dumped revision 0.\n",
    "WARNING 0x0002: E160005: "
      "While validating fspath '?\\E6': "
      "Path '?\\E6' is not in UTF-8"
      "\n",
    "* Dumped revision 1.\n",
    ]
  exit_code, output, errput = svntest.main.run_svnadmin("dump", sbox.repo_dir)
  if svntest.verify.compare_and_display_lines(
    "Output of 'svnadmin dump' is unexpected.",
    'STDERR', expected_stderr, errput):
    raise svntest.Failure

def test_lslocks_and_rmlocks(sbox):
  "test 'svnadmin lslocks' and 'svnadmin rmlocks'"

  sbox.build(create_wc=False)
  iota_url = sbox.repo_url + '/iota'
  lambda_url = sbox.repo_url + '/A/B/lambda'

  exit_code, output, errput = svntest.main.run_svnadmin("lslocks",
                                                        sbox.repo_dir)

  if exit_code or errput or output:
    raise svntest.Failure("Error: 'lslocks' failed")

  expected_output = svntest.verify.UnorderedRegexListOutput(
    ["'.*lambda' locked by user 'jrandom'.\n",
     "'.*iota' locked by user 'jrandom'.\n"])

  # Lock iota and A/B/lambda using svn client
  svntest.actions.run_and_verify_svn(None, expected_output,
                                     [], "lock", "-m", "Locking files",
                                     iota_url, lambda_url)

  def expected_output_list(path):
    return [
      "Path: " + path,
      "UUID Token: opaquelocktoken",
      "Owner: jrandom",
      "Created:",
      "Expires:",
      "Comment \(1 line\):",
      "Locking files",
      "\n", # empty line
      ]

  # List all locks
  exit_code, output, errput = svntest.main.run_svnadmin("lslocks",
                                                        sbox.repo_dir)
  if errput:
    raise SVNUnexpectedStderr(errput)
  svntest.verify.verify_exit_code(None, exit_code, 0)

  expected_output = svntest.verify.UnorderedRegexListOutput(
                                     expected_output_list('/A/B/lambda') +
                                     expected_output_list('/iota'))
  svntest.verify.compare_and_display_lines('lslocks output mismatch',
                                           'output',
                                           expected_output, output)

  # List lock in path /A
  exit_code, output, errput = svntest.main.run_svnadmin("lslocks",
                                                        sbox.repo_dir,
                                                        "A")
  if errput:
    raise SVNUnexpectedStderr(errput)

  expected_output = svntest.verify.RegexListOutput(
                                     expected_output_list('/A/B/lambda'))
  svntest.verify.compare_and_display_lines('lslocks output mismatch',
                                           'output',
                                           expected_output, output)
  svntest.verify.verify_exit_code(None, exit_code, 0)

  # Remove locks
  exit_code, output, errput = svntest.main.run_svnadmin("rmlocks",
                                                        sbox.repo_dir,
                                                        "iota",
                                                        "A/B/lambda")
  expected_output = UnorderedOutput(["Removed lock on '/iota'.\n",
                                     "Removed lock on '/A/B/lambda'.\n"])

  svntest.verify.verify_outputs(
    "Unexpected output while running 'svnadmin rmlocks'.",
    output, [], expected_output, None)

#----------------------------------------------------------------------
@Issue(3734)
def load_ranges(sbox):
  "'svnadmin load --revision X:Y'"

  ## See http://subversion.tigris.org/issues/show_bug.cgi?id=3734. ##
  test_create(sbox)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svnadmin_tests_data',
                                   'skeleton_repos.dump')
  dumplines = open(dumpfile_location).readlines()
  dumpdata = "".join(dumplines)

  # Load our dumpfile, 2 revisions at a time, verifying that we have
  # the correct youngest revision after each load.
  load_dumpstream(sbox, dumpdata, '-r0:2')
  svntest.actions.run_and_verify_svnlook("Unexpected output", ['2\n'],
                                         None, 'youngest', sbox.repo_dir)
  load_dumpstream(sbox, dumpdata, '-r3:4')
  svntest.actions.run_and_verify_svnlook("Unexpected output", ['4\n'],
                                         None, 'youngest', sbox.repo_dir)
  load_dumpstream(sbox, dumpdata, '-r5:6')
  svntest.actions.run_and_verify_svnlook("Unexpected output", ['6\n'],
                                         None, 'youngest', sbox.repo_dir)

  # There are ordering differences in the property blocks.
  if (svntest.main.options.server_minor_version < 6):
    temp = []

    for line in dumplines:
      if not "Text-content-sha1:" in line:
        temp.append(line)

    expected_dump = UnorderedOutput(temp)
  else:
    expected_dump = UnorderedOutput(dumplines)

  new_dumpdata = svntest.actions.run_and_verify_dump(sbox.repo_dir)
  svntest.verify.compare_and_display_lines("Dump files", "DUMP",
                                           expected_dump, new_dumpdata)

@SkipUnless(svntest.main.is_fs_type_fsfs)
def hotcopy_incremental(sbox):
  "'svnadmin hotcopy --incremental PATH .'"
  sbox.build()

  backup_dir, backup_url = sbox.add_repo_path('backup')
  os.mkdir(backup_dir)
  cwd = os.getcwd()

  for i in [1, 2, 3]:
    os.chdir(backup_dir)
    svntest.actions.run_and_verify_svnadmin(
      None, None, [],
      "hotcopy", "--incremental", os.path.join(cwd, sbox.repo_dir), '.')

    os.chdir(cwd)

    check_hotcopy_fsfs(sbox.repo_dir, backup_dir)

    if i < 3:
      sbox.simple_mkdir("newdir-%i" % i)
      sbox.simple_commit()

@SkipUnless(svntest.main.is_fs_type_fsfs)
@SkipUnless(svntest.main.fs_has_pack)
def hotcopy_incremental_packed(sbox):
  "'svnadmin hotcopy --incremental' with packing"

  # Configure two files per shard to trigger packing.
  sbox.build()
  write_sharded_format(sbox.repo_dir, 2)

  backup_dir, backup_url = sbox.add_repo_path('backup')
  os.mkdir(backup_dir)
  cwd = os.getcwd()

  # Pack revisions 0 and 1.
  svntest.actions.run_and_verify_svnadmin(
    None, ['Packing revisions in shard 0...done.\n'], [], "pack",
    os.path.join(cwd, sbox.repo_dir))

  # Commit 5 more revs, hotcopy and pack after each commit.
  for i in [1, 2, 3, 4, 5]:
    os.chdir(backup_dir)
    svntest.actions.run_and_verify_svnadmin(
      None, None, [],
      "hotcopy", "--incremental", os.path.join(cwd, sbox.repo_dir), '.')

    os.chdir(cwd)

    check_hotcopy_fsfs(sbox.repo_dir, backup_dir)

    if i < 5:
      sbox.simple_mkdir("newdir-%i" % i)
      sbox.simple_commit()
      if not i % 2:
        expected_output = ['Packing revisions in shard %d...done.\n' % (i/2)]
      else:
        expected_output = []
      svntest.actions.run_and_verify_svnadmin(
        None, expected_output, [], "pack", os.path.join(cwd, sbox.repo_dir))


def locking(sbox):
  "svnadmin lock tests"
  sbox.build(create_wc=False)

  comment_path = os.path.join(svntest.main.temp_dir, "comment")
  svntest.main.file_write(comment_path, "dummy comment")

  invalid_comment_path = os.path.join(svntest.main.temp_dir, "invalid_comment")
  svntest.main.file_write(invalid_comment_path, "character  is invalid")

  # Test illegal character in comment file.
  expected_error = ".*svnadmin: E130004:.*"
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          expected_error, "lock",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          invalid_comment_path)

  # Test locking path with --bypass-hooks
  expected_output = "'iota' locked by user 'jrandom'."
  svntest.actions.run_and_verify_svnadmin(None, expected_output,
                                          None, "lock",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          comment_path,
                                          "--bypass-hooks")

  # Remove lock
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          None, "rmlocks",
                                          sbox.repo_dir, "iota")

  # Test locking path without --bypass-hooks
  expected_output = "'iota' locked by user 'jrandom'."
  svntest.actions.run_and_verify_svnadmin(None, expected_output,
                                          None, "lock",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          comment_path)

  # Test locking already locked path.
  expected_error = ".*svnadmin: E160035:.*"
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          expected_error, "lock",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          comment_path)

  # Test locking non-existent path.
  expected_error = ".*svnadmin: E160013:.*"
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          expected_error, "lock",
                                          sbox.repo_dir,
                                          "non-existent", "jrandom",
                                          comment_path)

  # Test locking a path while specifying a lock token.
  expected_output = "'A/D/G/rho' locked by user 'jrandom'."
  lock_token = "opaquelocktoken:01234567-89ab-cdef-89ab-cdef01234567"
  svntest.actions.run_and_verify_svnadmin(None, expected_output,
                                          None, "lock",
                                          sbox.repo_dir,
                                          "A/D/G/rho", "jrandom",
                                          comment_path, lock_token)

  # Test unlocking a path, but provide the wrong lock token.
  expected_error = ".*svnadmin: E160040:.*"
  wrong_lock_token = "opaquelocktoken:12345670-9ab8-defc-9ab8-def01234567c"
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          expected_error, "unlock",
                                          sbox.repo_dir,
                                          "A/D/G/rho", "jrandom",
                                          wrong_lock_token)

  # Test unlocking the path again, but this time provide the correct
  # lock token.
  expected_output = "'A/D/G/rho' unlocked."
  svntest.actions.run_and_verify_svnadmin(None, expected_output,
                                          None, "unlock",
                                          sbox.repo_dir,
                                          "A/D/G/rho", "jrandom",
                                          lock_token)

  # Install lock/unlock prevention hooks.
  hook_path = svntest.main.get_pre_lock_hook_path(sbox.repo_dir)
  svntest.main.create_python_hook_script(hook_path, 'import sys; sys.exit(1)')
  hook_path = svntest.main.get_pre_unlock_hook_path(sbox.repo_dir)
  svntest.main.create_python_hook_script(hook_path, 'import sys; sys.exit(1)')

  # Test locking a path.  Don't use --bypass-hooks, though, as we wish
  # to verify that hook script is really getting executed.
  expected_error = ".*svnadmin: E165001:.*"
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          expected_error, "lock",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          comment_path)

  # Fetch the lock token for our remaining locked path.  (We didn't
  # explicitly set it, so it will vary from test run to test run.)
  exit_code, output, errput = svntest.main.run_svnadmin("lslocks",
                                                        sbox.repo_dir,
                                                        "iota")
  iota_token = None
  for line in output:
    if line.startswith("UUID Token: opaquelocktoken:"):
      iota_token = line[12:].rstrip()
      break
  if iota_token is None:
    raise svntest.Failure("Unable to lookup lock token for 'iota'")

  # Try to unlock a path while providing the correct lock token but
  # with a preventative hook in place.
  expected_error = ".*svnadmin: E165001:.*"
  svntest.actions.run_and_verify_svnadmin(None, None,
                                          expected_error, "unlock",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          iota_token)

  # Finally, use --bypass-hooks to unlock the path (again using the
  # correct lock token).
  expected_output = "'iota' unlocked."
  svntest.actions.run_and_verify_svnadmin(None, expected_output,
                                          None, "unlock",
                                          "--bypass-hooks",
                                          sbox.repo_dir,
                                          "iota", "jrandom",
                                          iota_token)


@SkipUnless(svntest.main.is_threaded_python)
@Issue(4129)
def mergeinfo_race(sbox):
  "concurrent mergeinfo commits invalidate pred-count"
  sbox.build()

  wc_dir = sbox.wc_dir
  wc2_dir = sbox.add_wc_path('2')

  ## Create wc2.
  svntest.main.run_svn(None, 'checkout', '-q', sbox.repo_url, wc2_dir)

  ## Some random edits.
  svntest.main.run_svn(None, 'mkdir', sbox.ospath('d1', wc_dir))
  svntest.main.run_svn(None, 'mkdir', sbox.ospath('d2', wc2_dir))

  ## Set random mergeinfo properties.
  svntest.main.run_svn(None, 'ps', 'svn:mergeinfo', '/P:42', sbox.ospath('A', wc_dir))
  svntest.main.run_svn(None, 'ps', 'svn:mergeinfo', '/Q:42', sbox.ospath('iota', wc2_dir))

  def makethread(some_wc_dir):
    def worker():
      svntest.main.run_svn(None, 'commit', '-mm', some_wc_dir)
    return worker

  t1 = threading.Thread(None, makethread(wc_dir))
  t2 = threading.Thread(None, makethread(wc2_dir))

  # t2 will trigger the issue #4129 sanity check in fs_fs.c
  t1.start(); t2.start()

  t1.join(); t2.join()

  # Crude attempt to make sure everything worked.
  # TODO: better way to catch exceptions in the thread
  if svntest.actions.run_and_parse_info(sbox.repo_url)[0]['Revision'] != '3':
    raise svntest.Failure("one or both commits failed")


@Issue(4213)
@Skip(svntest.main.is_fs_type_fsx)
def recover_old_empty(sbox):
  "recover empty --compatible-version=1.3"
  svntest.main.safe_rmtree(sbox.repo_dir, 1)
  svntest.main.create_repos(sbox.repo_dir, minor_version=3)
  svntest.actions.run_and_verify_svnadmin(None, None, [],
                                          "recover", sbox.repo_dir)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def verify_keep_going(sbox):
  "svnadmin verify --keep-going test"

  sbox.build(create_wc = False)
  repo_url = sbox.repo_url
  B_url = sbox.repo_url + '/B'
  C_url = sbox.repo_url + '/C'

  # Create A/B/E/bravo in r2.
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir', '-m', 'log_msg',
                                     B_url)

  svntest.actions.run_and_verify_svn(None, None, [],
                                     'mkdir', '-m', 'log_msg',
                                     C_url)
  
  r2 = fsfs_file(sbox.repo_dir, 'revs', '2')
  fp = open(r2, 'r+b')
  fp.write("""inserting junk to corrupt the rev""")
  fp.close()
  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        "--keep-going",
                                                        sbox.repo_dir)

  exp_out = svntest.verify.RegexListOutput([".*Verified revision 0.",
                                            ".*Verified revision 1.",
                                            ".*Error verifying revision 2.",
                                            ".*Error verifying revision 3.",
                                            ".*",
                                            ".*Summary.*",
                                            ".*r2: E160004:.*",
                                            ".*r2: E160004:.*",
                                            ".*r3: E160004:.*",
                                            ".*r3: E160004:.*"])
  exp_err = svntest.verify.RegexListOutput(["svnadmin: E160004:.*",
                                            "svnadmin: E165011:.*"], False)

  if (svntest.main.fs_has_rep_sharing()):
    exp_out.insert(0, ".*Verifying.*metadata.*")

  if svntest.verify.verify_outputs("Unexpected error while running 'svnadmin verify'.",
                                   output, errput, exp_out, exp_err):
    raise svntest.Failure

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)

  if (svntest.main.is_fs_log_addressing()):
    exp_out = svntest.verify.RegexListOutput([".*Verifying metadata at revision 0"])
  else:
    exp_out = svntest.verify.RegexListOutput([".*Verified revision 0.",
                                             ".*Verified revision 1.",
                                             ".*Error verifying revision 2."])
    if (svntest.main.fs_has_rep_sharing()):
      exp_out.insert(0, ".*Verifying repository metadata.*")

  if svntest.verify.verify_outputs("Unexpected error while running 'svnadmin verify'.",
                                   output, errput, exp_out, exp_err):
    raise svntest.Failure


  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        "--quiet",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs("Output of 'svnadmin verify' is unexpected.",
                                   None, errput, None, "svnadmin: E165011:.*"):
    raise svntest.Failure

@SkipUnless(svntest.main.is_fs_type_fsfs)
def verify_invalid_path_changes(sbox):
  "detect invalid changed path list entries"

  sbox.build(create_wc = False)
  repo_url = sbox.repo_url

  # Create a number of revisions each adding a single path
  for r in range(2,20):
    svntest.actions.run_and_verify_svn(None, None, [],
                                       'mkdir', '-m', 'log_msg',
                                       sbox.repo_url + '/B' + str(r))

  # modify every other revision to make sure that errors are not simply
  # "carried over" but that all corrupts we get detected independently

  # add existing node
  set_changed_path_list(sbox, 2,
                        "_0.0.t1-1 add-dir false false /A\n\n")

  # add into non-existent parent
  set_changed_path_list(sbox, 4,
                        "_0.0.t3-2 add-dir false false /C/X\n\n")

  # del non-existent node
  set_changed_path_list(sbox, 6,
                        "_0.0.t5-2 delete-dir false false /C\n\n")

  # del existent node of the wrong kind
  #
  # THIS WILL NOT BE DETECTED
  # since dump mechanism and file don't care about the types of deleted nodes
  set_changed_path_list(sbox, 8,
                        "_0.0.t7-2 delete-file false false /B3\n\n")

  # copy from non-existent node
  set_changed_path_list(sbox, 10,
                        "_0.0.t9-2 add-dir false false /B10\n"
                        "6 /B8\n")

  # copy from existing node of the wrong kind
  set_changed_path_list(sbox, 12,
                        "_0.0.t11-2 add-file false false /B12\n"
                        "9 /B8\n")

  # modify non-existent node
  set_changed_path_list(sbox, 14,
                        "_0.0.t13-2 modify-file false false /A/D/H/foo\n\n")

  # modify existent node of the wrong kind
  set_changed_path_list(sbox, 16,
                        "_0.0.t15-2 modify-file false false /B12\n\n")

  # replace non-existent node
  set_changed_path_list(sbox, 18,
                        "_0.0.t17-2 replace-file false false /A/D/H/foo\n\n")

  # find corruptions
  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        "--keep-going",
                                                        sbox.repo_dir)

  exp_out = svntest.verify.RegexListOutput([".*Verified revision 0.",
                                           ".*Verified revision 1.",
                                           ".*Error verifying revision 2.",
                                           ".*Verified revision 3.",
                                           ".*Error verifying revision 4.",
                                           ".*Verified revision 5.",
                                           ".*Error verifying revision 6.",
                                           ".*Verified revision 7.",
                                           ".*Verified revision 8.",
                                           ".*Verified revision 9.",
                                           ".*Error verifying revision 10.",
                                           ".*Verified revision 11.",
                                           ".*Error verifying revision 12.",
                                           ".*Verified revision 13.",
                                           ".*Error verifying revision 14.",
                                           ".*Verified revision 15.",
                                           ".*Error verifying revision 16.",
                                           ".*Verified revision 17.",
                                           ".*Error verifying revision 18.",
                                           ".*Verified revision 19.",
                                           ".*",
                                           ".*Summary.*",
                                           ".*r2: E160020:.*",
                                           ".*r2: E160020:.*",
                                           ".*r4: E160013:.*",
                                           ".*r6: E160013:.*",
                                           ".*r6: E160013:.*",
                                           ".*r10: E160013:.*",
                                           ".*r10: E160013:.*",
                                           ".*r12: E145001:.*",
                                           ".*r12: E145001:.*",
                                           ".*r14: E160013:.*",
                                           ".*r14: E160013:.*",
                                           ".*r16: E145001:.*",
                                           ".*r16: E145001:.*",
                                           ".*r18: E160013:.*",
                                           ".*r18: E160013:.*"])
  if (svntest.main.fs_has_rep_sharing()):
    exp_out.insert(0, ".*Verifying.*metadata.*")
    if svntest.main.is_fs_log_addressing():
      exp_out.insert(1, ".*Verifying.*metadata.*")

  exp_err = svntest.verify.RegexListOutput(["svnadmin: E160020:.*",
                                            "svnadmin: E145001:.*",
                                            "svnadmin: E160013:.*"], False)


  if svntest.verify.verify_outputs("Unexpected error while running 'svnadmin verify'.",
                                   output, errput, exp_out, exp_err):
    raise svntest.Failure

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir)

  exp_out = svntest.verify.RegexListOutput([".*Verified revision 0.",
                                            ".*Verified revision 1.",
                                            ".*Error verifying revision 2."])
  exp_err = svntest.verify.RegexListOutput(["svnadmin: E160020:.*",
                                            "svnadmin: E165011:.*"], False)

  if (svntest.main.fs_has_rep_sharing()):
    exp_out.insert(0, ".*Verifying.*metadata.*")
    if svntest.main.is_fs_log_addressing():
      exp_out.insert(1, ".*Verifying.*metadata.*")
  if svntest.verify.verify_outputs("Unexpected error while running 'svnadmin verify'.",
                                   output, errput, exp_out, exp_err):
    raise svntest.Failure


  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        "--quiet",
                                                        sbox.repo_dir)

  if svntest.verify.verify_outputs("Output of 'svnadmin verify' is unexpected.",
                                   None, errput, None, "svnadmin: E165011:.*"):
    raise svntest.Failure


def verify_denormalized_names(sbox):
  "detect denormalized names and name collisions"

  sbox.build(create_wc = False)
  svntest.main.safe_rmtree(sbox.repo_dir, True)
  svntest.main.create_repos(sbox.repo_dir)

  dumpfile_location = os.path.join(os.path.dirname(sys.argv[0]),
                                   'svnadmin_tests_data',
                                   'normalization_check.dump')
  load_dumpstream(sbox, open(dumpfile_location).read())

  exit_code, output, errput = svntest.main.run_svnadmin(
    "verify", "--check-normalization", sbox.repo_dir)

  expected_output_regex_list = [
    ".*Verified revision 0.",
    ".*Verified revision 1.",
    ".*Verified revision 2.",
    ".*Verified revision 3.",
                                           # A/{Eacute}/{aring}lpha
    "WARNING 0x0003: Duplicate representation of path 'A/.*/.*lpha'",
    ".*Verified revision 4.",
    ".*Verified revision 5.",
                                                      # Q/{aring}lpha
    "WARNING 0x0004: Duplicate representation of path '/Q/.*lpha'"
                                  # A/{Eacute}
    " in svn:mergeinfo property of 'A/.*'",
    ".*Verified revision 6.",
    ".*Verified revision 7."]

  # The BDB backend doesn't do global metadata verification.
  if (svntest.main.fs_has_rep_sharing()):
    expected_output_regex_list.insert(0, ".*Verifying repository metadata.*")

  if svntest.main.is_fs_log_addressing():
    expected_output_regex_list.insert(0, ".* Verifying metadata at revision 0.*")

  exp_out = svntest.verify.RegexListOutput(expected_output_regex_list)
  exp_err = svntest.verify.ExpectedOutput([])

  svntest.verify.verify_outputs(
    "Unexpected error while running 'svnadmin verify'.",
    output, errput, exp_out, exp_err)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_recover_old_non_empty(sbox):
  "fsfs recover non-empty --compatible-version=1.3"

  # Around trunk@1560210, 'svnadmin recover' wrongly errored out
  # for the --compatible-version=1.3 Greek tree repository:
  # svnadmin: E200002: Serialized hash missing terminator

  sbox.build(create_wc=False, minor_version=3)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "recover",
                                          sbox.repo_dir)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_hotcopy_old_non_empty(sbox):
  "fsfs hotcopy non-empty --compatible-version=1.3"

  # Around trunk@1560210, 'svnadmin hotcopy' wrongly errored out
  # for the --compatible-version=1.3 Greek tree repository:
  # svnadmin: E160006: No such revision 1

  sbox.build(create_wc=False, minor_version=3)
  backup_dir, backup_url = sbox.add_repo_path('backup')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)

  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)


def load_ignore_dates(sbox):
  "svnadmin load --ignore-dates"

  # All revisions in the loaded repository should come after this time.
  start_time = time.localtime()
  time.sleep(1)
  
  sbox.build(create_wc=False)
  svntest.main.safe_rmtree(sbox.repo_dir, True)
  svntest.main.create_repos(sbox.repo_dir)

  dumpfile_skeleton = open(os.path.join(os.path.dirname(sys.argv[0]),
                                        'svnadmin_tests_data',
                                        'skeleton_repos.dump')).read()

  load_dumpstream(sbox, dumpfile_skeleton, '--ignore-dates')
  svntest.actions.run_and_verify_svnlook("Unexpected output", ['6\n'],
                                         None, 'youngest', sbox.repo_dir)
  for rev in range(6):
    exit_code, output, errput = svntest.main.run_svnlook('date', '-r', rev,
                                                         sbox.repo_dir)
    if errput:
      raise SVNUnexpectedStderr(errput)
    rev_time = time.strptime(output[0].rstrip()[:19], '%Y-%m-%d %H:%M:%S')
    if rev_time < start_time:
      raise svntest.Failure("Revision time for r%d older than load start time\n"
                            "    rev_time: %s\n"
                            "  start_time: %s"
                            % (rev, str(rev_time), str(start_time)))


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_hotcopy_old_with_id_changes(sbox):
  "fsfs hotcopy old with node-id and copy-id changes"

  # Around trunk@1573728, running 'svnadmin hotcopy' for the
  # --compatible-version=1.3 repository with certain node-id and copy-id
  # changes ended with mismatching db/current in source and destination:
  #
  #   source: "2 l 1"  destination: "2 k 1",
  #           "3 l 2"               "3 4 2"
  #           (and so on...)
  #
  # We test this case by creating a --compatible-version=1.3 repository
  # and committing things that result in node-id and copy-id changes.
  # After every commit, we hotcopy the repository to a new destination
  # and check whether the source of the backup and the backup itself are
  # identical.  We also maintain a separate --incremental backup, which
  # is updated and checked after every commit.
  sbox.build(create_wc=True, minor_version=3)

  inc_backup_dir, inc_backup_url = sbox.add_repo_path('incremental-backup')

  # r1 = Initial greek tree sandbox.
  backup_dir, backup_url = sbox.add_repo_path('backup-after-r1')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)

  # r2 = Add a new property.
  sbox.simple_propset('foo', 'bar', 'A/mu')
  sbox.simple_commit(message='r2')

  backup_dir, backup_url = sbox.add_repo_path('backup-after-r2')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)

  # r3 = Copy a file.
  sbox.simple_copy('A/B/E', 'A/B/E1')
  sbox.simple_commit(message='r3')

  backup_dir, backup_url = sbox.add_repo_path('backup-after-r3')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)

  # r4 = Remove an existing file ...
  sbox.simple_rm('A/D/gamma')
  sbox.simple_commit(message='r4')

  backup_dir, backup_url = sbox.add_repo_path('backup-after-r4')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)

  # r5 = ...and replace it with a new file here.
  sbox.simple_add_text("This is the replaced file.\n", 'A/D/gamma')
  sbox.simple_commit(message='r5')

  backup_dir, backup_url = sbox.add_repo_path('backup-after-r5')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)

  # r6 = Add an entirely new file.
  sbox.simple_add_text('This is an entirely new file.\n', 'A/C/mu1')
  sbox.simple_commit(message='r6')

  backup_dir, backup_url = sbox.add_repo_path('backup-after-r6')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)

  # r7 = Change the content of the existing file (this changeset does
  #      not bump the next-id and copy-id counters in the repository).
  sbox.simple_append('A/mu', 'This is change in the existing file.\n')
  sbox.simple_commit(message='r7')

  backup_dir, backup_url = sbox.add_repo_path('backup-after-r7')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          "--incremental",
                                          sbox.repo_dir, inc_backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, backup_dir)
  check_hotcopy_fsfs(sbox.repo_dir, inc_backup_dir)


@SkipUnless(svntest.main.fs_has_pack)
def verify_packed(sbox):
  "verify packed with small shards"

  # Configure two files per shard to trigger packing.
  sbox.build()
  write_sharded_format(sbox.repo_dir, 2)

  # Play with our greek tree.  These changesets fall into two
  # separate shards with r2 and r3 being in shard 1 ...
  sbox.simple_append('iota', "Line.\n")
  sbox.simple_append('A/D/gamma', "Another line.\n")
  sbox.simple_commit(message='r2')
  sbox.simple_propset('foo', 'bar', 'iota')
  sbox.simple_propset('foo', 'baz', 'A/mu')
  sbox.simple_commit(message='r3')

  # ...and r4 and r5 being in shard 2.
  sbox.simple_rm('A/C')
  sbox.simple_copy('A/B/E', 'A/B/E1')
  sbox.simple_move('A/mu', 'A/B/mu')
  sbox.simple_commit(message='r4')
  sbox.simple_propdel('foo', 'A/B/mu')
  sbox.simple_commit(message='r5')

  if svntest.main.is_fs_type_fsfs and svntest.main.options.fsfs_packing:
    # With --fsfs-packing, everything is already packed and we
    # can skip this part.
    pass
  else:
    expected_output = ["Packing revisions in shard 0...done.\n",
                       "Packing revisions in shard 1...done.\n",
                       "Packing revisions in shard 2...done.\n"]
    svntest.actions.run_and_verify_svnadmin(None, expected_output, [],
                                            "pack", sbox.repo_dir)

  if svntest.main.is_fs_log_addressing():
    expected_output = ["* Verifying metadata at revision 0 ...\n",
                       "* Verifying metadata at revision 2 ...\n",
                       "* Verifying metadata at revision 4 ...\n",
                       "* Verifying repository metadata ...\n",
                       "* Verified revision 0.\n",
                       "* Verified revision 1.\n",
                       "* Verified revision 2.\n",
                       "* Verified revision 3.\n",
                       "* Verified revision 4.\n",
                       "* Verified revision 5.\n"]
  else:
    expected_output = ["* Verifying repository metadata ...\n",
                       "* Verified revision 0.\n",
                       "* Verified revision 1.\n",
                       "* Verified revision 2.\n",
                       "* Verified revision 3.\n",
                       "* Verified revision 4.\n",
                       "* Verified revision 5.\n"]

  svntest.actions.run_and_verify_svnadmin(None, expected_output, [],
                                          "verify", sbox.repo_dir)

# Test that 'svnadmin freeze' is nestable.  (For example, this ensures it
# won't take system-global locks, only repository-scoped ones.)
#
# This could be useful to easily freeze a small number of repositories at once.
#
# ### We don't actually test that freeze takes a write lock anywhere (not even
# ### in C tests.)
def freeze_freeze(sbox):
  "svnadmin freeze svnadmin freeze (some-cmd)"

  sbox.build(create_wc=False, read_only=True)
  second_repo_dir, _ = sbox.add_repo_path('backup')
  svntest.actions.run_and_verify_svnadmin(None, None, [], "hotcopy",
                                          sbox.repo_dir, second_repo_dir)
  svntest.actions.run_and_verify_svnadmin(None, [], None,
                                          'setuuid', second_repo_dir)

  svntest.actions.run_and_verify_svnadmin(None, None, [],
                 'freeze', '--', sbox.repo_dir,
                 svntest.main.svnadmin_binary, 'freeze', '--', second_repo_dir,
                 sys.executable, '-c', 'True')

  arg_file = sbox.get_tempname()
  svntest.main.file_write(arg_file,
                          "%s\n%s\n" % (sbox.repo_dir, second_repo_dir))

  svntest.actions.run_and_verify_svnadmin(None, None, [],
                                          'freeze', '-F', arg_file, '--',
                                          sys.executable, '-c', 'True')

def verify_metadata_only(sbox):
  "verify metadata only"

  sbox.build(create_wc = False)
  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir,
                                                        "--metadata-only")
  if errput:
    raise SVNUnexpectedStderr(errput)

  # Unfortunately, older formats won't test as thoroughly than newer ones
  # resulting in different progress output. BDB will do a full check but
  # not produce any output.
  if svntest.main.is_fs_log_addressing():
    svntest.verify.compare_and_display_lines(
      "Unexpected error while running 'svnadmin verify'.",
      'STDOUT', ["* Verifying metadata at revision 0 ...\n",
                 "* Verifying repository metadata ...\n"], output)
  elif svntest.main.fs_has_rep_sharing() \
       and not svntest.main.is_fs_type_bdb():
    svntest.verify.compare_and_display_lines(
      "Unexpected error while running 'svnadmin verify'.",
      'STDOUT', ["* Verifying repository metadata ...\n"], output)
  else:
    svntest.verify.compare_and_display_lines(
      "Unexpected error while running 'svnadmin verify'.",
      'STDOUT', [], output)


@Skip(svntest.main.is_fs_type_bdb)
def verify_quickly(sbox):
  "verify quickly using metadata"

  sbox.build(create_wc = False)
  rev_file = open(fsfs_file(sbox.repo_dir, 'revs', '1'), 'r+b')

  # set new contents
  rev_file.seek(8)
  rev_file.write('#')
  rev_file.close()

  exit_code, output, errput = svntest.main.run_svnadmin("verify",
                                                        sbox.repo_dir,
                                                        "--metadata-only")

  # unfortunately, some backends needs to do more checks than other
  # resulting in different progress output
  if svntest.main.is_fs_log_addressing():
    exp_out = svntest.verify.RegexListOutput([])
    exp_err = svntest.verify.RegexListOutput(["svnadmin: E160004:.*",
                                              "svnadmin: E165011:.*"], False)
  else:
    exp_out = svntest.verify.RegexListOutput([])
    exp_err = svntest.verify.RegexListOutput([])

  if (svntest.main.fs_has_rep_sharing()):
    exp_out.insert(0, ".*Verifying.*metadata.*")
  if svntest.verify.verify_outputs("Unexpected error while running 'svnadmin verify'.",
                                   output, errput, exp_out, exp_err):
    raise svntest.Failure


@SkipUnless(svntest.main.is_fs_type_fsfs)
@SkipUnless(svntest.main.fs_has_pack)
def fsfs_hotcopy_progress(sbox):
  "hotcopy progress reporting"

  # Check how 'svnadmin hotcopy' reports progress for non-incremental
  # and incremental scenarios.  The progress output can be affected by
  # the --fsfs-packing option, so skip the test if that is the case.
  if svntest.main.options.fsfs_packing:
    raise svntest.Skip

  # Create an empty repository, configure three files per shard.
  sbox.build(create_wc=False)
  svntest.main.safe_rmtree(sbox.repo_dir, True)
  svntest.main.create_repos(sbox.repo_dir)
  write_sharded_format(sbox.repo_dir, 3)

  inc_backup_dir, inc_backup_url = sbox.add_repo_path('incremental-backup')

  # Nothing really exciting for the empty repository.
  expected_full = [
    "* Copied revision 0.\n"
    ]
  expected_incremental = [
    "* Copied revision 0.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup-0')
  svntest.actions.run_and_verify_svnadmin(None, expected_full, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, expected_incremental, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, inc_backup_dir)

  # Commit three revisions.  After this step we have a full shard
  # (r0, r1, r2) and the second shard (r3) with a single revision.
  for i in range(3):
    svntest.actions.run_and_verify_svn(None, None, [], 'mkdir',
                                       '-m', svntest.main.make_log_msg(),
                                       sbox.repo_url + '/dir-%i' % i)
  expected_full = [
    "* Copied revision 0.\n",
    "* Copied revision 1.\n",
    "* Copied revision 2.\n",
    "* Copied revision 3.\n",
    ]
  expected_incremental = [
    "* Copied revision 1.\n",
    "* Copied revision 2.\n",
    "* Copied revision 3.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup-1')
  svntest.actions.run_and_verify_svnadmin(None, expected_full, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, expected_incremental, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, inc_backup_dir)

  # Pack everything (r3 is still unpacked) and hotcopy again.  In this case,
  # the --incremental output should track the incoming (r0, r1, r2) pack and
  # should not mention r3, because it is already a part of the destination
  # and is *not* a part of the incoming pack.
  svntest.actions.run_and_verify_svnadmin(None, None, [], 'pack',
                                          sbox.repo_dir)
  expected_full = [
    "* Copied revisions from 0 to 2.\n",
    "* Copied revision 3.\n",
    ]
  expected_incremental = [
    "* Copied revisions from 0 to 2.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup-2')
  svntest.actions.run_and_verify_svnadmin(None, expected_full, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, expected_incremental, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, inc_backup_dir)

  # Fill the second shard, pack again, commit several unpacked revisions
  # on top of it.  Rerun the hotcopy and check the progress output.
  for i in range(4, 6):
    svntest.actions.run_and_verify_svn(None, None, [], 'mkdir',
                                       '-m', svntest.main.make_log_msg(),
                                       sbox.repo_url + '/dir-%i' % i)

  svntest.actions.run_and_verify_svnadmin(None, None, [], 'pack',
                                          sbox.repo_dir)

  for i in range(6, 8):
    svntest.actions.run_and_verify_svn(None, None, [], 'mkdir',
                                       '-m', svntest.main.make_log_msg(),
                                       sbox.repo_url + '/dir-%i' % i)
  expected_full = [
    "* Copied revisions from 0 to 2.\n",
    "* Copied revisions from 3 to 5.\n",
    "* Copied revision 6.\n",
    "* Copied revision 7.\n",
    ]
  expected_incremental = [
    "* Copied revisions from 3 to 5.\n",
    "* Copied revision 6.\n",
    "* Copied revision 7.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup-3')
  svntest.actions.run_and_verify_svnadmin(None, expected_full, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, expected_incremental, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, inc_backup_dir)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_hotcopy_progress_with_revprop_changes(sbox):
  "incremental hotcopy progress with changed revprops"

  # The progress output can be affected by the --fsfs-packing
  # option, so skip the test if that is the case.
  if svntest.main.options.fsfs_packing:
    raise svntest.Skip

  # Create an empty repository, commit several revisions and hotcopy it.
  sbox.build(create_wc=False)
  svntest.main.safe_rmtree(sbox.repo_dir, True)
  svntest.main.create_repos(sbox.repo_dir)

  for i in range(6):
    svntest.actions.run_and_verify_svn(None, None, [], 'mkdir',
                                       '-m', svntest.main.make_log_msg(),
                                       sbox.repo_url + '/dir-%i' % i)
  expected_output = [
    "* Copied revision 0.\n",
    "* Copied revision 1.\n",
    "* Copied revision 2.\n",
    "* Copied revision 3.\n",
    "* Copied revision 4.\n",
    "* Copied revision 5.\n",
    "* Copied revision 6.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup')
  svntest.actions.run_and_verify_svnadmin(None, expected_output, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)

  # Amend a few log messages in the source, run the --incremental hotcopy.
  # The progress output should only mention the corresponding revisions.
  revprop_file = sbox.get_tempname()
  svntest.main.file_write(revprop_file, "Modified log message.")

  for i in [1, 3, 6]:
    svntest.actions.run_and_verify_svnadmin(None, None, [],
                                            'setrevprop',
                                            sbox.repo_dir, '-r', i,
                                            'svn:log', revprop_file)
  expected_output = [
    "* Copied revision 1.\n",
    "* Copied revision 3.\n",
    "* Copied revision 6.\n",
    ]
  svntest.actions.run_and_verify_svnadmin(None, expected_output, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, backup_dir)


@SkipUnless(svntest.main.is_fs_type_fsfs)
def fsfs_hotcopy_progress_old(sbox):
  "hotcopy --compatible-version=1.3 progress"

  sbox.build(create_wc=False)
  svntest.main.safe_rmtree(sbox.repo_dir, True)
  svntest.main.create_repos(sbox.repo_dir, minor_version=3)

  inc_backup_dir, inc_backup_url = sbox.add_repo_path('incremental-backup')

  # Nothing really exciting for the empty repository.
  expected_full = [
    "* Copied revision 0.\n"
    ]
  expected_incremental = [
    "* Copied revision 0.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup-0')
  svntest.actions.run_and_verify_svnadmin(None, expected_full, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, expected_incremental, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, inc_backup_dir)

  # Commit three revisions, hotcopy and check the progress output.
  for i in range(3):
    svntest.actions.run_and_verify_svn(None, None, [], 'mkdir',
                                       '-m', svntest.main.make_log_msg(),
                                       sbox.repo_url + '/dir-%i' % i)

  expected_full = [
    "* Copied revision 0.\n",
    "* Copied revision 1.\n",
    "* Copied revision 2.\n",
    "* Copied revision 3.\n",
    ]
  expected_incremental = [
    "* Copied revision 1.\n",
    "* Copied revision 2.\n",
    "* Copied revision 3.\n",
    ]

  backup_dir, backup_url = sbox.add_repo_path('backup-1')
  svntest.actions.run_and_verify_svnadmin(None, expected_full, [],
                                          'hotcopy',
                                          sbox.repo_dir, backup_dir)
  svntest.actions.run_and_verify_svnadmin(None, expected_incremental, [],
                                          'hotcopy', '--incremental',
                                          sbox.repo_dir, inc_backup_dir)


########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              extra_headers,
              extra_blockcontent,
              inconsistent_headers,
              empty_date,
              dump_copied_dir,
              dump_move_dir_modify_child,
              dump_quiet,
              hotcopy_dot,
              hotcopy_format,
              setrevprop,
              verify_windows_paths_in_repos,
              verify_incremental_fsfs,
              fsfs_recover_db_current,
              fsfs_recover_old_db_current,
              load_with_parent_dir,
              set_uuid,
              reflect_dropped_renumbered_revs,
              fsfs_recover_handle_missing_revs_or_revprops_file,
              create_in_repo_subdir,
              verify_with_invalid_revprops,
              dont_drop_valid_mergeinfo_during_incremental_loads,
              hotcopy_symlink,
              load_bad_props,
              verify_non_utf8_paths,
              test_lslocks_and_rmlocks,
              load_ranges,
              hotcopy_incremental,
              hotcopy_incremental_packed,
              locking,
              mergeinfo_race,
              recover_old_empty,
              verify_keep_going,
              verify_invalid_path_changes,
              verify_denormalized_names,
              fsfs_recover_old_non_empty,
              fsfs_hotcopy_old_non_empty,
              load_ignore_dates,
              fsfs_hotcopy_old_with_id_changes,
              verify_packed,
              freeze_freeze,
              verify_metadata_only,
              verify_quickly,
              fsfs_hotcopy_progress,
              fsfs_hotcopy_progress_with_revprop_changes,
              fsfs_hotcopy_progress_old,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.
