#!/usr/bin/env python
#
#  log_tests.py:  testing "svn log"
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2003 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import string, sys, re, os, shutil

# Our testing module
import svntest


######################################################################
#
# The Plan:
#
# Get a repository, commit about 6 or 7 revisions to it, each
# involving different kinds of operations.  Make sure to have some
# add, del, mv, cp, as well as file modifications, and make sure that
# some files are modified more than once.
#
# Give each commit a recognizable log message.  Test all combinations
# of -r options, including none.  Then test with -v, which will
# (presumably) show changed paths as well.
#
######################################################################



######################################################################
# Globals
#

# These variables are set by guarantee_repos_and_wc().
max_revision = 0    # Highest revision in the repos

# What separates log msgs from one another in raw log output.
msg_separator = '------------------------------------' \
                + '------------------------------------\n'


# (abbreviation)
Skip = svntest.testcase.Skip
XFail = svntest.testcase.XFail
Item = svntest.wc.StateItem


######################################################################
# Utilities
#

def guarantee_repos_and_wc(sbox):
  "Make a repos and wc, commit max_revision revs."
  global max_revision

  sbox.build()

  wc_path = sbox.wc_dir

  # Now we have a repos and wc at revision 1.

  was_cwd = os.getcwd ()
  os.chdir (wc_path)

  # Set up the paths we'll be using most often.
  iota_path = os.path.join ('iota')
  mu_path = os.path.join ('A', 'mu')
  B_path = os.path.join ('A', 'B')
  omega_path = os.path.join ('A', 'D', 'H', 'omega')
  pi_path = os.path.join ('A', 'D', 'G', 'pi')
  rho_path = os.path.join ('A', 'D', 'G', 'rho')
  alpha_path = os.path.join ('A', 'B', 'E', 'alpha')
  beta_path = os.path.join ('A', 'B', 'E', 'beta')
  psi_path = os.path.join ('A', 'D', 'H', 'psi')
  epsilon_path = os.path.join ('A', 'C', 'epsilon')

  # Do a varied bunch of commits.  No copies yet, we'll wait till Ben
  # is done for that.

  # Revision 2: edit iota
  svntest.main.file_append (iota_path, "2")
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 2")
  svntest.main.run_svn (None, 'up')

  # Revision 3: edit A/D/H/omega, A/D/G/pi, A/D/G/rho, and A/B/E/alpha
  svntest.main.file_append (omega_path, "3")
  svntest.main.file_append (pi_path, "3")
  svntest.main.file_append (rho_path, "3")
  svntest.main.file_append (alpha_path, "3")
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 3")
  svntest.main.run_svn (None, 'up')

  # Revision 4: edit iota again, add A/C/epsilon
  svntest.main.file_append (iota_path, "4")
  svntest.main.file_append (epsilon_path, "4")
  svntest.main.run_svn (None, 'add', epsilon_path)
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 4")
  svntest.main.run_svn (None, 'up')

  # Revision 5: edit A/C/epsilon, delete A/D/G/rho
  svntest.main.file_append (epsilon_path, "5")
  svntest.main.run_svn (None, 'rm', rho_path)
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 5")
  svntest.main.run_svn (None, 'up')

  # Revision 6: prop change on A/B, edit A/D/H/psi
  svntest.main.run_svn (None, 'ps', 'blue', 'azul', B_path)  
  svntest.main.file_append (psi_path, "6")
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 6")
  svntest.main.run_svn (None, 'up')

  # Revision 7: edit A/mu, prop change on A/mu
  svntest.main.file_append (mu_path, "7")
  svntest.main.run_svn (None, 'ps', 'red', 'burgundy', mu_path)
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 7")
  svntest.main.run_svn (None, 'up')

  # Revision 8: edit iota yet again, re-add A/D/G/rho
  svntest.main.file_append (iota_path, "8")
  svntest.main.file_append (rho_path, "8")
  svntest.main.run_svn (None, 'add', rho_path)
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 8")
  svntest.main.run_svn (None, 'up')

  # Revision 9: edit A/B/E/beta, delete A/B/E/alpha
  svntest.main.file_append (beta_path, "9")
  svntest.main.run_svn (None, 'rm', alpha_path)
  svntest.main.run_svn (None, 'ci', '-m', "Log message for revision 9")
  svntest.main.run_svn (None, 'up')

  max_revision = 9

  # Restore.
  os.chdir (was_cwd)

  # Let's run 'svn status' and make sure the working copy looks
  # exactly the way we think it should.  Start with a generic
  # greek-tree-list, where every local and repos revision is at 9.
  expected_status = svntest.actions.get_virginal_state(wc_path, 9)
  expected_status.remove('A/B/E/alpha')
  expected_status.add({
    'A/C/epsilon' : Item(status='  ', wc_rev=9, repos_rev=9),
    })

  # props exist on A/B and A/mu
  expected_status.tweak('A/B', 'A/mu', status='  ')

  # Run 'svn st -uv' and compare the actual results with our tree.
  svntest.actions.run_and_verify_status(wc_path, expected_status)




# For errors seen while parsing log data.
class SVNLogParseError(Exception):
  def __init__ (self, args=None):
    self.args = args


def parse_log_output(log_lines):
  """Return a log chain derived from LOG_LINES.
  A log chain is a list of hashes; each hash represents one log
  message, in the order it appears in LOG_LINES (the first log
  message in the data is also the first element of the list, and so
  on).

  Each hash contains the following keys/values:

     'revision' ===>  number
     'author'   ===>  string
     'date'     ===>  string
     'msg'      ===>  string  (the log message itself)

  If LOG_LINES contains changed-path information, then the hash
  also contains

     'paths'    ===>  list of strings

     """

  # Here's some log output to look at while writing this function:
  
  # ------------------------------------------------------------------------
  # rev 5:  kfogel | Tue 6 Nov 2001 17:18:19 | 1 line
  # 
  # Log message for revision 5.
  # ------------------------------------------------------------------------
  # rev 4:  kfogel | Tue 6 Nov 2001 17:18:18 | 1 line
  # 
  # Log message for revision 4.
  # ------------------------------------------------------------------------
  # rev 3:  kfogel | Tue 6 Nov 2001 17:18:17 | 1 line
  # 
  # Log message for revision 3.
  # ------------------------------------------------------------------------
  # rev 2:  kfogel | Tue 6 Nov 2001 17:18:16 | 1 line
  # 
  # Log message for revision 2.
  # ------------------------------------------------------------------------
  # rev 1:  foo | Tue 6 Nov 2001 15:27:57 | 1 line
  # 
  # Log message for revision 1.
  # ------------------------------------------------------------------------

  # Regular expression to match the header line of a log message, with
  # these groups: (revision number), (author), (date), (num lines).
  header_re = re.compile ('^rev ([0-9]+):  ' \
                          + '([^|]*) \| ([^|]*) \| ([0-9]+) lines?')

  # The log chain to return.
  chain = []

  this_item = None
  while 1:
    try:
      this_line = log_lines.pop (0)
    except IndexError:
      return chain

    match = header_re.search (this_line)
    if match and match.groups ():
      this_item = {}
      this_item['revision'] = match.group(1)
      this_item['author']   = match.group(2)
      this_item['date']     = match.group(3)
      lines = string.atoi ((match.group (4)))

      # Eat the expected blank line.
      log_lines.pop (0)

      ### todo: we don't parse changed-paths yet, since Subversion
      ### doesn't output them.  When it does, they'll appear here,
      ### right after the header line, and then there'll be a blank
      ### line between them and the msg.

      # Accumulate the log message
      msg = ''
      for line in log_lines[0:lines]:
        msg += line
      del log_lines[0:lines]
    elif this_line == msg_separator:
      if this_item:
        this_item['msg'] = msg
        chain.append (this_item)
    else:  # if didn't see separator now, then something's wrong
      raise SVNLogParseError, "trailing garbage after log message"

  return chain


def check_log_chain (chain, revlist):
  """Verify that log chain CHAIN contains the right log messages for
  revisions START to END (see documentation for parse_log_output() for
  more about log chains.)

  Do nothing if the log chain's messages run from revision START to END
  with no gaps, and that each log message is one line of the form

     'Log message for revision N'

  where N is the revision number of that commit.  Also verify that
  author and date are present and look sane, but don't check them too
  carefully.

  Raise if anything looks wrong.
  """

  for expect_rev in revlist:
    log_item = chain.pop (0)
    saw_rev = string.atoi (log_item['revision'])
    date = log_item['date']
    author = log_item['author']
    msg = log_item['msg']
    # The most important check is that the revision is right:
    if expect_rev != saw_rev: raise svntest.Failure
    # Check that date looks at least vaguely right:
    date_re = re.compile ('[0-9]+')
    if (not date_re.search (date)): raise svntest.Failure
    # Authors are a little harder, since they might not exist over ra-dav.
    # Well, it's not much of a check, but we'll do what we can.
    author_re = re.compile ('[a-zA-Z]+')
    if (not (author_re.search (author)
             or author == ''
             or author == '(no author)')): raise svntest.Failure
    # Check that the log message looks right:
    msg_re = re.compile ('Log message for revision ' + `saw_rev`)
    if (not msg_re.search (msg)): raise svntest.Failure

  ### todo: need some multi-line log messages mixed in with the
  ### one-liners.  Easy enough, just make the prime revisions use REV
  ### lines, and the rest use 1 line, or something, so it's
  ### predictable based on REV.


######################################################################
# Tests
#

#----------------------------------------------------------------------
def plain_log(sbox):
  "'svn log', no args, top of wc"

  guarantee_repos_and_wc(sbox)

  result = 0

  was_cwd = os.getcwd()
  os.chdir(sbox.wc_dir)

  output, errput = svntest.main.run_svn (None, 'log')

  if errput:
    os.chdir (was_cwd)
    raise svntest.Failure

  log_chain = parse_log_output (output)
  if check_log_chain (log_chain, range(max_revision, 1 - 1, -1)):
    os.chdir (was_cwd)
    raise svntest.Failure

  os.chdir (was_cwd)


#----------------------------------------------------------------------
def versioned_log_message(sbox):
  "'svn commit -F foo' when foo is a versioned file"

  sbox.build()

  was_cwd = os.getcwd ()
  os.chdir (sbox.wc_dir)

  iota_path = os.path.join ('iota')
  mu_path = os.path.join ('A', 'mu')
  log_path = os.path.join ('A', 'D', 'H', 'omega')

  svntest.main.file_append (iota_path, "2")

  # try to check in a change using a versioned file as your log entry.
  stdout_lines, stderr_lines = svntest.main.run_svn (1, 'ci', '-F', log_path)

  # make sure we failed.
  if (len(stderr_lines) <= 0):
    os.chdir (was_cwd)
    raise svntest.Failure

  # force it.  should not produce any errors.
  stdout_lines, stderr_lines = \
    svntest.main.run_svn (None, 'ci', '-F', log_path, '--force-log')

  if (len(stderr_lines) != 0):
    os.chdir (was_cwd)
    raise svntest.Failure

  svntest.main.file_append (mu_path, "2")

  # try the same thing, but specifying the file to commit explicitly.
  stdout_lines, stderr_lines = \
    svntest.main.run_svn (1, 'ci', '-F', log_path, mu_path)

  # make sure it failed.
  if (len(stderr_lines) <= 0):
    os.chdir (was_cwd)
    raise svntest.Failure

  # force it...  should succeed.
  stdout_lines, stderr_lines = \
    svntest.main.run_svn (None, 'ci', '-F', log_path, '--force-log', mu_path)

  if (len(stderr_lines) != 0):
    os.chdir (was_cwd)
    raise svntest.Failure

  os.chdir (was_cwd)


#----------------------------------------------------------------------
def log_with_empty_repos(sbox):
  "'svn log' on an empty repository"

  # Create virgin repos
  if os.path.exists(sbox.repo_dir):
    shutil.rmtree(sbox.repo_dir)
  svntest.main.create_repos(sbox.repo_dir)
  svntest.main.set_repos_paths(sbox.repo_dir)

  stdout_lines, stderr_lines = svntest.main.run_svn\
                               (None, 'log',
                                '--username', svntest.main.wc_author,
                                '--password', svntest.main.wc_passwd,
                                svntest.main.current_repo_url)

  if (len(stderr_lines) != 0):
    raise svntest.Failure


#----------------------------------------------------------------------
def log_where_nothing_changed(sbox):
  "'svn log -rN some_dir_unchanged_in_N'"
  sbox.build()

  # Fix bug whereby running 'svn log -rN SOMEPATH' would result in an
  # xml protocol error if there were no changes in revision N
  # underneath SOMEPATH.  This problem was introduced in revision
  # 3811, which didn't cover the case where svn_repos_get_logs might
  # invoke log_receiver zero times.  Since the receiver never ran, the
  # lrb->needs_header flag never got cleared.  Control would proceed
  # without error to the end of dav_svn__log_report(), which would
  # send a closing tag even though no opening tag had ever been sent.

  rho_path = os.path.join (sbox.wc_dir, 'A', 'D', 'G', 'rho')
  svntest.main.file_append (rho_path, "some new material in rho")
  svntest.actions.run_and_verify_svn(None, None, [], 'ci', '-m',
                                     'log msg', rho_path)

  # Now run 'svn log -r2' on a directory unaffected by revision 2.
  H_path = os.path.join (sbox.wc_dir, 'A', 'D', 'H')
  svntest.actions.run_and_verify_svn(None, None, [],
                                     'log', '-r', '2', H_path)


#----------------------------------------------------------------------
def log_to_revision_zero(sbox):
  "'svn log -v -r 1:0 wc_root'"
  sbox.build()

  # This used to the segfault the server.
  stdout_lines, stderr_lines = svntest.main.run_svn(0, 'log', '-v',
                                                    '-r', '1:0', sbox.wc_dir)
  if stderr_lines:
    raise svntest.Failure


#----------------------------------------------------------------------
def log_with_path_args(sbox):
  "'svn log', no args, top of wc"

  guarantee_repos_and_wc(sbox)

  result = 0

  was_cwd = os.getcwd()
  os.chdir(sbox.wc_dir)

  output, errput = svntest.main.run_svn (None, 'log',
                                         svntest.main.current_repo_url,
                                         'A/D/G', 'A/D/H')

  if errput:
    os.chdir (was_cwd)
    raise svntest.Failure

  log_chain = parse_log_output (output)
  if check_log_chain (log_chain, [8, 6, 5, 3, 1]):
    os.chdir (was_cwd)
    raise svntest.Failure

  os.chdir (was_cwd)

#----------------------------------------------------------------------
def url_missing_in_head(sbox):
  "'svn log -r N URL' when URL is not in HEAD "

  guarantee_repos_and_wc(sbox)

  result = 0

  my_url = svntest.main.current_repo_url + "/A/B/E/alpha"
  output, errput = svntest.main.run_svn (None, 'log', '-r', '8', my_url)

  if errput:
    os.chdir (was_cwd)
    raise svntest.Failure

#----------------------------------------------------------------------
def setup_copyfrom_history(sbox):
  "setup repository with copyfrom history"
  sbox.build()
  wc_dir = sbox.wc_dir

  mu_path = os.path.join (wc_dir, 'A', 'mu')
  mu2_path = os.path.join (wc_dir, 'A', 'mu2')
  mu_URL = svntest.main.current_repo_url + '/A/mu'
  mu2_URL = svntest.main.current_repo_url + '/A/mu2'

  svntest.main.file_append (mu_path, "2")
  svntest.actions.run_and_verify_svn (None, None, [], 'ci', wc_dir,
                                      '-m', "Log message for revision 2")
  svntest.main.file_append (mu_path, "3")
  svntest.actions.run_and_verify_svn (None, None, [], 'ci', wc_dir,
                                      '-m', "Log message for revision 3")

  svntest.actions.run_and_verify_svn (None, None, [],
                                      'cp', '-r', '2', mu_URL, mu2_URL,
                                      '-m', "Log message for revision 4")

def log_through_copyfrom_history(sbox):
  "'svn log URL' with copyfrom history"
  setup_copyfrom_history(sbox)
  mu2_URL = svntest.main.current_repo_url + '/A/mu2'
  output, errput = svntest.main.run_svn (None, 'log', mu2_URL)
  if errput:
    raise svntest.Failure
  log_chain = parse_log_output (output)
  if check_log_chain (log_chain, [4, 2, 1]):
    raise svntest.Failure

# XFail because it tries to get /A/mu2 at rev 2
def log_rN_through_copyfrom_history(sbox):
  "'svn log -rN URL' with copyfrom history"
  setup_copyfrom_history(sbox)
  mu2_URL = svntest.main.current_repo_url + '/A/mu2'
  output, errput = svntest.main.run_svn (None, 'log', '-r', '2', mu2_URL)
  if errput:
    raise svntest.Failure
  log_chain = parse_log_output (output)
  if check_log_chain (log_chain, [2]):
    raise svntest.Failure
  
########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              plain_log,
              versioned_log_message,
              log_with_empty_repos,
              log_where_nothing_changed,
              log_to_revision_zero,
              log_with_path_args,
              url_missing_in_head,
              log_through_copyfrom_history,
              XFail(log_rN_through_copyfrom_history),
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.
