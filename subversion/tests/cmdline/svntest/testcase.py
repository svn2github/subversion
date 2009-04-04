#
#  testcase.py:  Control of test case execution.
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2000-2004, 2008-2009 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import os, types

import svntest

# if somebody does a "from testcase import *", they only get these names
__all__ = ['XFail', 'Wimp', 'Skip', 'SkipUnless']

RESULT_OK = 'ok'
RESULT_FAIL = 'fail'
RESULT_SKIP = 'skip'


class TestCase:
  """A thing that can be tested.  This is an abstract class with
  several methods that need to be overridden."""

  _result_map = {
    RESULT_OK:   (0, 'PASS: ', True),
    RESULT_FAIL: (1, 'FAIL: ', False),
    RESULT_SKIP: (2, 'SKIP: ', True),
    }

  def __init__(self, delegate=None, cond_func=lambda: True, doc=None, wip=None):
    assert hasattr(cond_func, '__call__')

    self._delegate = delegate
    self._cond_func = cond_func
    self.description = doc or delegate.description
    self.inprogress = wip

  def get_sandbox_name(self):
    """Return the name that should be used for the sandbox.

    If a sandbox should not be constructed, this method returns None.
    """
    return self._delegate.get_sandbox_name()

  def run(self, sandbox):
    """Run the test within the given sandbox."""
    return self._delegate.run(sandbox)

  def list_mode(self):
    return ''

  def results(self, result):
    # if our condition applied, then use our result map. otherwise, delegate.
    if self._cond_func():
      return self._result_map[result]
    return self._delegate.results(result)


class FunctionTestCase(TestCase):
  """A TestCase based on a naked Python function object.

  FUNC should be a function that returns None on success and throws an
  svntest.Failure exception on failure.  It should have a brief
  docstring describing what it does (and fulfilling certain conditions).
  FUNC must take one argument, an Sandbox instance.  (The sandbox name
  is derived from the file name in which FUNC was defined)
  """

  def __init__(self, func):
    # it better be a function that accepts an sbox parameter and has a
    # docstring on it.
    assert isinstance(func, types.FunctionType)

    name = func.func_name

    assert func.func_code.co_argcount == 1, \
        '%s must take an sbox argument' % name

    doc = func.__doc__.strip()
    assert doc, '%s must have a docstring' % name

    # enforce stylistic guidelines for the function docstrings:
    # - no longer than 50 characters
    # - should not end in a period
    # - should not be capitalized
    assert len(doc) <= 50, \
        "%s's docstring must be 50 characters or less" % name
    assert doc[-1] != '.', \
        "%s's docstring should not end in a period" % name
    assert doc[0].lower() == doc[0], \
        "%s's docstring should not be capitalized" % name

    TestCase.__init__(self, doc=doc)
    self.func = func

  def get_sandbox_name(self):
    """Base the sandbox's name on the name of the file in which the
    function was defined."""

    filename = self.func.func_code.co_filename
    return os.path.splitext(os.path.basename(filename))[0]

  def run(self, sandbox):
    return self.func(sandbox)


class XFail(TestCase):
  """A test that is expected to fail, if its condition is true."""

  _result_map = {
    RESULT_OK:   (1, 'XPASS:', False),
    RESULT_FAIL: (0, 'XFAIL:', True),
    RESULT_SKIP: (2, 'SKIP: ', True),
    }

  def __init__(self, test_case, cond_func=lambda: True, wip=None):
    """Create an XFail instance based on TEST_CASE.  COND_FUNC is a
    callable that is evaluated at test run time and should return a
    boolean value.  If COND_FUNC returns true, then TEST_CASE is
    expected to fail (and a pass is considered an error); otherwise,
    TEST_CASE is run normally.  The evaluation of COND_FUNC is
    deferred so that it can base its decision on useful bits of
    information that are not available at __init__ time (like the fact
    that we're running over a particular RA layer)."""

    TestCase.__init__(self, create_test_case(test_case), cond_func, wip=wip)

  def list_mode(self):
    # basically, the only possible delegate is a Skip test. favor that mode.
    return self._delegate.list_mode() or 'XFAIL'


class Wimp(XFail):
  """Like XFail, but indicates a work-in-progress: an unexpected pass
  is not considered a test failure."""

  _result_map = {
    RESULT_OK:   (0, 'XPASS:', True),
    RESULT_FAIL: (0, 'XFAIL:', True),
    RESULT_SKIP: (2, 'SKIP: ', True),
    }

  def __init__(self, wip, test_case, cond_func=lambda: True):
    XFail.__init__(self, test_case, cond_func, wip)


class Skip(TestCase):
  """A test that will be skipped if its conditional is true."""

  def __init__(self, test_case, cond_func=lambda: True):
    """Create an Skip instance based on TEST_CASE.  COND_FUNC is a
    callable that is evaluated at test run time and should return a
    boolean value.  If COND_FUNC returns true, then TEST_CASE is
    skipped; otherwise, TEST_CASE is run normally.
    The evaluation of COND_FUNC is deferred so that it can base its
    decision on useful bits of information that are not available at
    __init__ time (like the fact that we're running over a
    particular RA layer)."""

    TestCase.__init__(self, create_test_case(test_case), cond_func)

  def list_mode(self):
    if self._cond_func():
      return 'SKIP'
    return self._delegate.list_mode()

  def get_sandbox_name(self):
    if self._cond_func():
      return None
    return self._delegate.get_sandbox_name()

  def run(self, sandbox):
    if self._cond_func():
      raise svntest.Skip
    return self._delegate.run(sandbox)


class SkipUnless(Skip):
  """A test that will be skipped if its conditional is false."""

  def __init__(self, test_case, cond_func):
    Skip.__init__(self, test_case, lambda c=cond_func: not c())


def create_test_case(func):
  if isinstance(func, TestCase):
    return func
  else:
    return FunctionTestCase(func)
