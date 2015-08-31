#!/usr/bin/env python
#
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
#
#
# run_tests.py - run the tests in the regression test suite.
#

'''usage: python run_tests.py
            [--verbose] [--log-to-stdout] [--cleanup] [--parallel]
            [--url=<base-url>] [--http-library=<http-library>] [--enable-sasl]
            [--fs-type=<fs-type>] [--fsfs-packing] [--fsfs-sharding=<n>]
            [--list] [--milestone-filter=<regex>] [--mode-filter=<type>]
            [--server-minor-version=<version>] [--http-proxy=<host>:<port>]
            [--httpd-version=<version>]
            [--config-file=<file>] [--ssl-cert=<file>]
            [--exclusive-wc-locks] [--memcached-server=<url:port>]
            <abs_srcdir> <abs_builddir>
            <prog ...>

The optional flags and the first two parameters are passed unchanged
to the TestHarness constructor.  All other parameters are names of
test programs.

Each <prog> should be the full path (absolute or from the current directory)
and filename of a test program, optionally followed by '#' and a comma-
separated list of test numbers; the default is to run all the tests in it.
'''

import os, sys
import optparse, subprocess, imp, threading, traceback, exceptions
from datetime import datetime

# Ensure the compiled C tests use a known locale (Python tests set the locale
# explicitly).
os.environ['LC_ALL'] = 'C'

# Placeholder for the svntest module
svntest = None

class TextColors:
  '''Some ANSI terminal constants for output color'''
  ENDC = '\033[0;m'
  FAILURE = '\033[1;31m'
  SUCCESS = '\033[1;32m'

  @classmethod
  def disable(cls):
    cls.ENDC = ''
    cls.FAILURE = ''
    cls.SUCCESS = ''


def _get_term_width():
  'Attempt to discern the width of the terminal'
  # This may not work on all platforms, in which case the default of 80
  # characters is used.  Improvements welcomed.

  def ioctl_GWINSZ(fd):
    try:
      import fcntl, termios, struct, os
      cr = struct.unpack('hh', fcntl.ioctl(fd, termios.TIOCGWINSZ,
                                           struct.pack('hh', 0, 0)))
    except:
      return None
    return cr

  cr = None
  if not cr:
    try:
      cr = (os.environ['SVN_MAKE_CHECK_LINES'],
            os.environ['SVN_MAKE_CHECK_COLUMNS'])
    except:
      cr = None
  if not cr:
    cr = ioctl_GWINSZ(0) or ioctl_GWINSZ(1) or ioctl_GWINSZ(2)
  if not cr:
    try:
      fd = os.open(os.ctermid(), os.O_RDONLY)
      cr = ioctl_GWINSZ(fd)
      os.close(fd)
    except:
      pass
  if not cr:
    try:
      cr = (os.environ['LINES'], os.environ['COLUMNS'])
    except:
      cr = None
  if not cr:
    # Default
    if sys.platform == 'win32':
      cr = (25, 79)
    else:
      cr = (25, 80)
  return int(cr[1])


class TestHarness:
  '''Test harness for Subversion tests.
  '''

  def __init__(self, abs_srcdir, abs_builddir, logfile, faillogfile, opts):
    '''Construct a TestHarness instance.

    ABS_SRCDIR and ABS_BUILDDIR are the source and build directories.
    LOGFILE is the name of the log file. If LOGFILE is None, let tests
    print their output to stdout and stderr, and don't print a summary
    at the end (since there's no log file to analyze).
    OPTS are the options that will be sent to the tests.
    '''

    # Canonicalize the test base URL
    if opts.url is not None and opts.url[-1] == '/':
      opts.url = opts.url[:-1]

    # Make the configfile path absolute
    if opts.config_file is not None:
      opts.config_file = os.path.abspath(opts.config_file)

    # Parse out the FSFS version number
    if (opts.fs_type is not None
         and opts.fs_type.startswith('fsfs-v')):
      opts.fsfs_version = int(opts.fs_type[6:])
      opts.fs_type = 'fsfs'
    else:
      opts.fsfs_version = None

    self.srcdir = abs_srcdir
    self.builddir = abs_builddir
    self.logfile = logfile
    self.faillogfile = faillogfile
    self.log = None
    self.opts = opts

    if not sys.stdout.isatty() or sys.platform == 'win32':
      TextColors.disable()

  def _init_c_tests(self):
    cmdline = [None, None]   # Program name and source dir

    if self.opts.config_file is not None:
      cmdline.append('--config-file=' + self.opts.config_file)
    elif self.opts.memcached_server is not None:
      cmdline.append('--memcached-server=' + self.opts.memcached_server)

    if self.opts.url is not None:
      subdir = 'subversion/tests/cmdline/svn-test-work'
      cmdline.append('--repos-url=%s' % self.opts.url +
                        '/svn-test-work/repositories')
      cmdline.append('--repos-dir=%s'
                     % os.path.abspath(
                         os.path.join(self.builddir,
                                      subdir, 'repositories')))

      # Enable access for http
      if self.opts.url.startswith('http'):
        authzparent = os.path.join(self.builddir, subdir)
        if not os.path.exists(authzparent):
          os.makedirs(authzparent);
        open(os.path.join(authzparent, 'authz'), 'w').write('[/]\n'
                                                            '* = rw\n')

    # ### Support --repos-template
    if self.opts.list_tests is not None:
      cmdline.append('--list')
    if self.opts.verbose is not None:
      cmdline.append('--verbose')
    if self.opts.cleanup is not None:
      cmdline.append('--cleanup')
    if self.opts.fs_type is not None:
      cmdline.append('--fs-type=%s' % self.opts.fs_type)
    if self.opts.fsfs_version is not None:
      cmdline.append('--fsfs-version=%d' % self.opts.fsfs_version)
    if self.opts.server_minor_version is not None:
      cmdline.append('--server-minor-version=' +
                     self.opts.server_minor_version)
    if self.opts.mode_filter is not None:
      cmdline.append('--mode-filter=' + self.opts.mode_filter)
    if self.opts.parallel is not None:
      cmdline.append('--parallel')

    self.c_test_cmdline = cmdline


  def _init_py_tests(self, basedir):
    cmdline = ['--srcdir=%s' % self.srcdir]
    if self.opts.list_tests is not None:
      cmdline.append('--list')
    if self.opts.verbose is not None:
      cmdline.append('--verbose')
    if self.opts.cleanup is not None:
      cmdline.append('--cleanup')
    if self.opts.parallel is not None:
      if self.opts.parallel == 1:
        cmdline.append('--parallel')
      else:
        cmdline.append('--parallel-instances=%d' % self.opts.parallel)
    if self.opts.svn_bin is not None:
      cmdline.append('--bin=%s', self.opts.svn_bin)
    if self.opts.url is not None:
      cmdline.append('--url=%s' % self.opts.url)
    if self.opts.fs_type is not None:
      cmdline.append('--fs-type=%s' % self.opts.fs_type)
    if self.opts.http_library is not None:
      cmdline.append('--http-library=%s' % self.opts.http_library)
    if self.opts.fsfs_sharding is not None:
      cmdline.append('--fsfs-sharding=%d' % self.opts.fsfs_sharding)
    if self.opts.fsfs_packing is not None:
      cmdline.append('--fsfs-packing')
    if self.opts.fsfs_version is not None:
      cmdline.append('--fsfs-version=%d' % self.opts.fsfs_version)
    if self.opts.server_minor_version is not None:
      cmdline.append('--server-minor-version=%d' % self.opts.server_minor_version)
    if self.opts.dump_load_cross_check is not None:
      cmdline.append('--dump-load-cross-check')
    if self.opts.enable_sasl is not None:
      cmdline.append('--enable-sasl')
    if self.opts.config_file is not None:
      cmdline.append('--config-file=%s' % self.opts.config_file)
    if self.opts.milestone_filter is not None:
      cmdline.append('--milestone-filter=%s' % self.opts.milestone_filter)
    if self.opts.mode_filter is not None:
      cmdline.append('--mode-filter=%s' % self.opts.mode_filter)
    if self.opts.set_log_level is not None:
      cmdline.append('--set-log-level=%s' % self.opts.set_log_level)
    if self.opts.ssl_cert is not None:
      cmdline.append('--ssl-cert=%s' % self.opts.ssl_cert)
    if self.opts.http_proxy is not None:
      cmdline.append('--http-proxy=%s' % self.opts.http_proxy)
    if self.opts.http_proxy_username is not None:
      cmdline.append('--http-proxy-username=%s' % self.opts.http_proxy_username)
    if self.opts.http_proxy_password is not None:
      cmdline.append('--http-proxy-password=%s' % self.opts.http_proxy_password)
    if self.opts.httpd_version is not None:
      cmdline.append('--httpd-version=%s' % self.opts.httpd_version)
    if self.opts.exclusive_wc_locks is not None:
      cmdline.append('--exclusive-wc-locks')
    if self.opts.memcached_server is not None:
      cmdline.append('--memcached-server=%s' % self.opts.memcached_server)

    # The svntest module is very pedantic about the current working directory
    old_cwd = os.getcwd()
    try:
      os.chdir(basedir)
      sys.path.insert(0, os.path.abspath(os.path.join(self.srcdir, basedir)))

      global svntest
      __import__('svntest')
      __import__('svntest.main')
      __import__('svntest.testcase')
      svntest = sys.modules['svntest']
      svntest.main = sys.modules['svntest.main']
      svntest.testcase = sys.modules['svntest.testcase']

      if svntest.main.logger is None:
        import logging
        svntest.main.logger = logging.getLogger()
      svntest.main.parse_options(cmdline, optparse.SUPPRESS_USAGE)

      svntest.testcase.TextColors.disable()
    finally:
      os.chdir(old_cwd)

  def run(self, testlist):
    '''Run all test programs given in TESTLIST. Print a summary of results, if
       there is a log file. Return zero iff all test programs passed.'''
    self._open_log('w')
    failed = 0

    # Filter tests into Python and native groups and prepare arguments
    # for each group. The resulting list will contain tuples of
    # (program dir, program name, test numbers), where the test
    # numbers may be None.

    def split_nums(prog):
      test_nums = []
      if '#' in prog:
        prog, test_nums = prog.split('#')
        if test_nums:
          test_nums = test_nums.split(',')
      return prog, test_nums

    py_basedir = set()
    py_tests = []
    c_tests = []

    for prog in testlist:
      progpath, testnums = split_nums(prog)
      progdir, progbase = os.path.split(progpath)
      if progpath.endswith('.py'):
        py_basedir.add(progdir)
        py_tests.append((progdir, progbase, testnums))
      elif not self.opts.skip_c_tests:
        c_tests.append((progdir, progbase, testnums))

    # Initialize svntest.main.options for Python tests. Load the
    # svntest.main module from the Python test path.
    if len(py_tests):
      if len(py_basedir) > 1:
        sys.stderr.write('The test harness requires all Python tests'
                         ' to be in the same directory.')
        sys.exit(1)
      self._init_py_tests(list(py_basedir)[0])
      py_tests.sort(key=lambda x: x[1])

    # Create the common command line for C tests
    if len(c_tests):
      self._init_c_tests()
      c_tests.sort(key=lambda x: x[1])

    # Run the tests
    testlist = c_tests + py_tests
    testcount = len(testlist)
    for count, testcase in enumerate(testlist):
      failed = self._run_test(testcase, count, testcount) or failed

    if self.log is None:
      return failed

    # Open the log in binary mode because it can contain binary data
    # from diff_tests.py's testing of svnpatch. This may prevent
    # readlines() from reading the whole log because it thinks it
    # has encountered the EOF marker.
    self._open_log('rb')
    log_lines = self.log.readlines()

    # Remove \r characters introduced by opening the log as binary
    if sys.platform == 'win32':
      log_lines = [x.replace('\r', '') for x in log_lines]

    # Print the results, from least interesting to most interesting.

    # Helper for Work-In-Progress indications for XFAIL tests.
    wimptag = ' [[WIMP: '
    def printxfail(x):
      wip = x.find(wimptag)
      if 0 > wip:
        sys.stdout.write(x)
      else:
        sys.stdout.write('%s\n       [[%s'
                         % (x[:wip], x[wip + len(wimptag):]))

    if self.opts.list_tests:
      passed = [x for x in log_lines if x[8:13] == '     ']
    else:
      passed = [x for x in log_lines if x[:6] == 'PASS: ']

    if self.opts.list_tests:
      skipped = [x for x in log_lines if x[8:12] == 'SKIP']
    else:
      skipped = [x for x in log_lines if x[:6] == 'SKIP: ']

    if skipped and not self.opts.list_tests:
      print('At least one test was SKIPPED, checking ' + self.logfile)
      for x in skipped:
        sys.stdout.write(x)

    if self.opts.list_tests:
      xfailed = [x for x in log_lines if x[8:13] == 'XFAIL']
    else:
      xfailed = [x for x in log_lines if x[:6] == 'XFAIL:']
    if xfailed and not self.opts.list_tests:
      print('At least one test XFAILED, checking ' + self.logfile)
      for x in xfailed:
        printxfail(x)

    xpassed = [x for x in log_lines if x[:6] == 'XPASS:']
    if xpassed:
      print('At least one test XPASSED, checking ' + self.logfile)
      for x in xpassed:
        printxfail(x)

    failed_list = [x for x in log_lines if x[:6] == 'FAIL: ']
    if failed_list:
      print('At least one test FAILED, checking ' + self.logfile)
      for x in failed_list:
        sys.stdout.write(x)

    # Print summaries, from least interesting to most interesting.
    if self.opts.list_tests:
      print('Summary of test listing:')
    else:
      print('Summary of test results:')
    if passed:
      if self.opts.list_tests:
        print('  %d test%s are set to PASS'
              % (len(passed), 's'*min(len(passed) - 1, 1)))
      else:
        print('  %d test%s PASSED'
              % (len(passed), 's'*min(len(passed) - 1, 1)))
    if skipped:
      if self.opts.list_tests:
        print('  %d test%s are set as SKIP'
              % (len(skipped), 's'*min(len(skipped) - 1, 1)))
      else:
        print('  %d test%s SKIPPED'
              % (len(skipped), 's'*min(len(skipped) - 1, 1)))
    if xfailed:
      passwimp = [x for x in xfailed if 0 <= x.find(wimptag)]
      if passwimp:
        if self.opts.list_tests:
          print('  %d test%s are set to XFAIL (%d WORK-IN-PROGRESS)'
                % (len(xfailed), 's'*min(len(xfailed) - 1, 1), len(passwimp)))
        else:
          print('  %d test%s XFAILED (%d WORK-IN-PROGRESS)'
                % (len(xfailed), 's'*min(len(xfailed) - 1, 1), len(passwimp)))
      else:
        if self.opts.list_tests:
          print('  %d test%s are set as XFAIL'
                % (len(xfailed), 's'*min(len(xfailed) - 1, 1)))
        else:
          print('  %d test%s XFAILED'
                % (len(xfailed), 's'*min(len(xfailed) - 1, 1)))
    if xpassed:
      failwimp = [x for x in xpassed if 0 <= x.find(wimptag)]
      if failwimp:
        print('  %d test%s XPASSED (%d WORK-IN-PROGRESS)'
              % (len(xpassed), 's'*min(len(xpassed) - 1, 1), len(failwimp)))
      else:
        print('  %d test%s XPASSED'
              % (len(xpassed), 's'*min(len(xpassed) - 1, 1)))
    if failed_list:
      print('  %d test%s FAILED'
            % (len(failed_list), 's'*min(len(failed_list) - 1, 1)))

    # Copy the truly interesting verbose logs to a separate file, for easier
    # viewing.
    if xpassed or failed_list:
      faillog = open(self.faillogfile, 'wb')
      last_start_lineno = None
      last_start_re = re.compile('^(FAIL|SKIP|XFAIL|PASS|START|CLEANUP|END):')
      for lineno, line in enumerate(log_lines):
        # Iterate the lines.  If it ends a test we're interested in, dump that
        # test to FAILLOG.  If it starts a test (at all), remember the line
        # number (in case we need it later).
        if line in xpassed or line in failed_list:
          faillog.write('[[[\n')
          faillog.writelines(log_lines[last_start_lineno : lineno+1])
          faillog.write(']]]\n\n')
        if last_start_re.match(line):
          last_start_lineno = lineno + 1
      faillog.close()
    elif os.path.exists(self.faillogfile):
      print("WARNING: no failures, but '%s' exists from a previous run."
            % self.faillogfile)

    # Summary.
    if failed or xpassed or failed_list:
      print("SUMMARY: Some tests failed.\n")
    else:
      print("SUMMARY: All tests successful.\n")

    self._close_log()
    return failed

  def _open_log(self, mode):
    'Open the log file with the required MODE.'
    if self.logfile:
      self._close_log()
      self.log = open(self.logfile, mode)

  def _close_log(self):
    'Close the log file.'
    if not self.log is None:
      self.log.close()
      self.log = None

  def _run_c_test(self, progabs, progdir, progbase, test_nums, dot_count):
    'Run a c test, escaping parameters as required.'
    if self.opts.list_tests and self.opts.milestone_filter:
      print 'WARNING: --milestone-filter option does not currently work with C tests'

    if not os.access(progbase, os.X_OK):
      print("\nNot an executable file: " + progbase)
      sys.exit(1)

    cmdline = self.c_test_cmdline[:]
    cmdline[0] = './' + progbase
    cmdline[1] = '--srcdir=%s' % os.path.join(self.srcdir, progdir)

    if test_nums:
      cmdline.extend(test_nums)
      total = len(test_nums)
    else:
      total_cmdline = [cmdline[0], '--list']
      prog = subprocess.Popen(total_cmdline, stdout=subprocess.PIPE)
      lines = prog.stdout.readlines()
      total = len(lines) - 2

    # This has to be class-scoped for use in the progress_func()
    self.dots_written = 0
    def progress_func(completed):
      if not self.log or self.dots_written >= dot_count:
        return
      dots = (completed * dot_count) / total
      if dots > dot_count:
        dots = dot_count
      dots_to_write = dots - self.dots_written
      os.write(sys.stdout.fileno(), '.' * dots_to_write)
      self.dots_written = dots

    tests_completed = 0
    prog = subprocess.Popen(cmdline, stdout=subprocess.PIPE,
                            stderr=self.log)
    line = prog.stdout.readline()
    while line:
      if sys.platform == 'win32':
        # Remove CRs inserted because we parse the output as binary.
        line = line.replace('\r', '')

      # If using --log-to-stdout self.log in None.
      if self.log:
        self.log.write(line)

      if line.startswith('PASS') or line.startswith('FAIL') \
           or line.startswith('XFAIL') or line.startswith('XPASS') \
           or line.startswith('SKIP'):
        tests_completed += 1
        progress_func(tests_completed)

      line = prog.stdout.readline()

    # If we didn't run any tests, still print out the dots
    if not tests_completed:
      os.write(sys.stdout.fileno(), '.' * dot_count)

    prog.wait()
    return prog.returncode

  def _run_py_test(self, progabs, progdir, progbase, test_nums, dot_count):
    'Run a python test, passing parameters as needed.'
    try:
      prog_mod = imp.load_module(progbase[:-3], open(progabs, 'r'), progabs,
                                 ('.py', 'U', imp.PY_SOURCE))
    except:
      print("\nError loading test (details in following traceback): " + progbase)
      traceback.print_exc()
      sys.exit(1)

    # setup the output pipes
    if self.log:
      sys.stdout.flush()
      sys.stderr.flush()
      self.log.flush()
      old_stdout = os.dup(sys.stdout.fileno())
      old_stderr = os.dup(sys.stderr.fileno())
      os.dup2(self.log.fileno(), sys.stdout.fileno())
      os.dup2(self.log.fileno(), sys.stderr.fileno())

    # These have to be class-scoped for use in the progress_func()
    self.dots_written = 0
    self.progress_lock = threading.Lock()
    def progress_func(completed, total):
      """Report test suite progress. Can be called from multiple threads
         in parallel mode."""
      if not self.log:
        return
      dots = (completed * dot_count) / total
      if dots > dot_count:
        dots = dot_count
      self.progress_lock.acquire()
      if self.dots_written < dot_count:
        dots_to_write = dots - self.dots_written
        self.dots_written = dots
        os.write(old_stdout, '.' * dots_to_write)
      self.progress_lock.release()

    serial_only = hasattr(prog_mod, 'serial_only') and prog_mod.serial_only

    # run the tests
    if self.opts.list_tests:
      prog_f = None
    else:
      prog_f = progress_func

    try:
      failed = svntest.main.execute_tests(prog_mod.test_list,
                                          serial_only=serial_only,
                                          test_name=progbase,
                                          progress_func=prog_f,
                                          test_selection=test_nums)
    except svntest.Failure:
      if self.log:
        os.write(old_stdout, '.' * dot_count)
      failed = True

    # restore some values
    if self.log:
      sys.stdout.flush()
      sys.stderr.flush()
      os.dup2(old_stdout, sys.stdout.fileno())
      os.dup2(old_stderr, sys.stderr.fileno())
      os.close(old_stdout)
      os.close(old_stderr)

    return failed

  def _run_test(self, testcase, test_nr, total_tests):
    "Run a single test. Return the test's exit code."

    if self.log:
      log = self.log
    else:
      log = sys.stdout

    progdir, progbase, test_nums = testcase
    if self.log:
      # Using write here because we don't want even a trailing space
      test_info = '[%s/%d] %s' % (str(test_nr + 1).zfill(len(str(total_tests))),
                                  total_tests, progbase)
      if self.opts.list_tests:
        sys.stdout.write('Listing tests in %s' % (test_info, ))
      else:
        sys.stdout.write('%s' % (test_info, ))
      sys.stdout.flush()
    else:
      # ### Hack for --log-to-stdout to work (but not print any dots).
      test_info = ''

    if self.opts.list_tests:
      log.write('LISTING: %s\n' % progbase)
    else:
      log.write('START: %s\n' % progbase)

    log.flush()

    start_time = datetime.now()

    progabs = os.path.abspath(os.path.join(self.srcdir, progdir, progbase))
    old_cwd = os.getcwd()
    line_length = _get_term_width()
    dots_needed = line_length \
                    - len(test_info) \
                    - len('success')
    try:
      os.chdir(progdir)
      if progbase[-3:] == '.py':
        testcase = self._run_py_test
      else:
        testcase = self._run_c_test
      failed = testcase(progabs, progdir, progbase, test_nums, dots_needed)
    except:
      os.chdir(old_cwd)
      raise
    else:
      os.chdir(old_cwd)

    # We always return 1 for failed tests. Some other failure than 1
    # probably means the test didn't run at all and probably didn't
    # output any failure info. In that case, log a generic failure message.
    # ### Even if failure==1 it could be that the test didn't run at all.
    if failed and failed != 1:
      if self.log:
        log.write('FAIL:  %s: Unknown test failure; see tests.log.\n' % progbase)
        log.flush()
      else:
        log.write('FAIL:  %s: Unknown test failure.\n' % progbase)

    if not self.opts.list_tests:
      # Log the elapsed time.
      elapsed_time = str(datetime.now() - start_time)
      log.write('END: %s\n' % progbase)
      log.write('ELAPSED: %s %s\n' % (progbase, elapsed_time))

    log.write('\n')

    # If we are only listing the tests just add a newline, otherwise if
    # we printed a "Running all tests in ..." line, add the test result.
    if self.log:
      if self.opts.list_tests:
        print ''
      else:
        if failed:
          print(TextColors.FAILURE + 'FAILURE' + TextColors.ENDC)
        else:
          print(TextColors.SUCCESS + 'success' + TextColors.ENDC)

    return failed


def create_parser():
  parser = optparse.OptionParser(usage=__doc__);

  parser.add_option('-l', '--list', action='store_true', dest='list_tests',
                    help='Print test doc strings instead of running them')
  parser.add_option('-v', '--verbose', action='store_true',
                    help='Print binary command-lines')
  parser.add_option('-c', '--cleanup', action='store_true',
                    help='Clean up after successful tests')
  parser.add_option('-p', '--parallel', action='store', type='int',
                    help='Run the tests in parallel')
  parser.add_option('-u', '--url', action='store',
                    help='Base url to the repos (e.g. svn://localhost)')
  parser.add_option('-f', '--fs-type', action='store',
                    help='Subversion file system type (fsfs(-v[46]), bdb or fsx)')
  parser.add_option('--http-library', action='store',
                    help="Make svn use this DAV library (neon or serf)")
  parser.add_option('--bin', action='store', dest='svn_bin',
                    help='Use the svn binaries installed in this path')
  parser.add_option('--fsfs-sharding', action='store', type='int',
                    help='Default shard size (for fsfs)')
  parser.add_option('--fsfs-packing', action='store_true',
                    help="Run 'svnadmin pack' automatically")
  parser.add_option('--server-minor-version', type='int', action='store',
                    help="Set the minor version for the server")
  parser.add_option('--skip-c-tests', '--skip-C-tests', action='store_true',
                    help="Run only the Python tests")
  parser.add_option('--dump-load-cross-check', action='store_true',
                    help="After every test, run a series of dump and load " +
                         "tests with svnadmin, svnrdump and svndumpfilter " +
                         " on the testcase repositories to cross-check " +
                         " dump file compatibility.")
  parser.add_option('--enable-sasl', action='store_true',
                    help='Whether to enable SASL authentication')
  parser.add_option('--config-file', action='store',
                    help="Configuration file for tests.")
  parser.add_option('--log-to-stdout', action='store_true',
                    help='Print test progress to stdout instead of a log file')
  parser.add_option('--milestone-filter', action='store', dest='milestone_filter',
                    help='Limit --list to those with target milestone specified')
  parser.add_option('--mode-filter', action='store', dest='mode_filter',
                    default='ALL',
                    help='Limit tests to those with type specified (e.g. XFAIL)')
  parser.add_option('--set-log-level', action='store',
                    help="Set log level (numerically or symbolically). " +
                         "Symbolic levels are: CRITICAL, ERROR, WARNING, " +
                         "INFO, DEBUG")
  parser.add_option('--ssl-cert', action='store',
                    help='Path to SSL server certificate.')
  parser.add_option('--http-proxy', action='store',
                    help='Use the HTTP Proxy at hostname:port.')
  parser.add_option('--http-proxy-username', action='store',
                    help='Username for the HTTP Proxy.')
  parser.add_option('--http-proxy-password', action='store',
                    help='Password for the HTTP Proxy.')
  parser.add_option('--httpd-version', action='store',
                    help='Assume HTTPD is this version.')
  parser.add_option('--exclusive-wc-locks', action='store_true',
                    help='Use sqlite exclusive locking for working copies')
  parser.add_option('--memcached-server', action='store',
                    help='Use memcached server at specified URL (FSFS only)')
  return parser

def main():
  (opts, args) = create_parser().parse_args(sys.argv[1:])

  if len(args) < 3:
    print(__doc__)
    sys.exit(2)

  if opts.log_to_stdout:
    logfile = None
    faillogfile = None
  else:
    logfile = os.path.abspath('tests.log')
    faillogfile = os.path.abspath('fails.log')

  th = TestHarness(args[0], args[1], logfile, faillogfile, opts)
  failed = th.run(args[2:])
  if failed:
    sys.exit(1)


# Run main if not imported as a module
if __name__ == '__main__':
  main()
