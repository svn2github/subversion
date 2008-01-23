#!/usr/bin/python

# ====================================================================
# Copyright (c) 2008 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
# This software consists of voluntary contributions made by many
# individuals.  For exact contribution history, see the revision
# history and logs, available at http://subversion.tigris.org/.
# ====================================================================

import os
import sys
import tempfile
import unittest

import svn.core

import svn_dav_log_parse

class TestCase(unittest.TestCase):
    def setUp(self):
        # Define a class to stuff everything passed to any handle_
        # method into self.result.
        class cls(svn_dav_log_parse.Parser):
            def __getattr__(cls_self, attr):
                if attr.startswith('handle_'):
                    return lambda *a: setattr(self, 'result', a)
                raise AttributeError
        self.parse = cls().parse

    def test_unknown(self):
        line = 'unknown log line'
        self.parse(line)
        self.assertEqual(self.result, (line,))

    def test_commit(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'commit')
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'commit 3')
        self.assertEqual(self.parse('commit r3'), '')
        self.assertEqual(self.result, (3,))
        self.assertEqual(self.parse('commit r3 leftover'), ' leftover')
        self.assertEqual(self.result, (3,))

    def test_list_dir(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'list-dir')
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'list-dir foo')
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'list-dir foo 3')
        self.assertEqual(self.parse('list-dir /a/b/c r3 ...'), ' ...')
        self.assertEqual(self.result, ('/a/b/c', 3))
        self.assertEqual(self.parse('list-dir / r3'), '')
        self.assertEqual(self.result, ('/', 3))
        # path must be absolute
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'list-dir a/b/c r3')

    def test_lock(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'lock')
        self.parse('lock /foo')
        self.assertEqual(self.result, ('/foo', False))
        self.assertEqual(self.parse('lock /foo steal ...'), ' ...')
        self.assertEqual(self.result, ('/foo', True))
        self.assertEqual(self.parse('lock /foo stear'), ' stear')

    def test_prop_list(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'prop-list')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'prop-list /foo')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'prop-list /foo@bar')
        self.assertEqual(self.parse('prop-list /foo@3 ...'), ' ...')
        self.assertEqual(self.result, ('/foo', 3))

    def test_revprop_change(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'revprop-change r3')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'revprop-change r svn:log')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'revprop-change rX svn:log')
        self.assertEqual(self.parse('revprop-change r3 svn:log ...'), ' ...')
        self.assertEqual(self.result, (3, 'svn:log'))

    def test_revprop_list(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'revprop-list')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'revprop-list r')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'revprop-list rX')
        self.assertEqual(self.parse('revprop-list r3 ...'), ' ...')
        self.assertEqual(self.result, (3,))

    def test_unlock(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'unlock')
        self.parse('unlock /foo')
        self.assertEqual(self.result, ('/foo', False))
        self.assertEqual(self.parse('unlock /foo break ...'), ' ...')
        self.assertEqual(self.result, ('/foo', True))
        self.assertEqual(self.parse('unlock /foo bear'), ' bear')

    def test_blame(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'blame')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'blame /foo 3')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'blame /foo 3:a')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'blame /foo r3:a')
        self.assertEqual(self.parse('blame /foo r3:4 ...'), ' ...')
        self.assertEqual(self.result, ('/foo', 3, 4, False))
        self.assertEqual(self.parse('blame /foo r3:4'
                                    ' include-merged-revisions ...'), ' ...')
        self.assertEqual(self.result, ('/foo', 3, 4, True))

    def test_get_mergeinfo(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'get-mergeinfo')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'get-mergeinfo /foo')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'get-mergeinfo (/foo')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'get-mergeinfo (/foo /bar')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'get-mergeinfo (/foo)')
        self.assertRaises(svn_dav_log_parse.BadMergeinfoInheritanceError,
                          self.parse, 'get-mergeinfo (/foo) bork')
        self.assertEqual(self.parse('get-mergeinfo (/foo) explicit'), '')
        self.assertEqual(self.result, (['/foo'],
                                       svn.core.svn_mergeinfo_explicit))
        self.assertEqual(self.parse('get-mergeinfo (/foo /bar) inherited ...'),
                         ' ...')
        self.assertEqual(self.result, (['/foo', '/bar'],
                                       svn.core.svn_mergeinfo_inherited))

    def test_log(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'log')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'log /foo')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'log (/foo)')
        self.assertEqual(self.parse('log (/foo) r3:4'
                                    ' include-merged-revisions'), '')
        self.assertEqual(self.result,
                         (['/foo'], 3, 4, 0, False, False, True, []))
        self.assertEqual(self.parse('log (/foo /bar) r3:4 revprops=all ...'),
                         ' ...')
        self.assertEqual(self.result,
                         (['/foo', '/bar'], 3, 4, 0, False, False, False, None))
        self.assertEqual(self.parse('log (/foo) r3:4 revprops=(a b) ...'),
                         ' ...')
        self.assertEqual(self.result,
                         (['/foo'], 3, 4, 0, False, False, False, ['a', 'b']))
        self.assertEqual(self.parse('log (/foo) r8:1 limit=3'), '')
        self.assertEqual(self.result,
                         (['/foo'], 8, 1, 3, False, False, False, []))

    def test_replay(self):
        self.assertRaises(svn_dav_log_parse.Error, self.parse, 'replay')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'replay /foo')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'replay (/foo) r9')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'replay (/foo) r9:10')
        self.assertEqual(self.parse('replay /foo r9'), '')
        self.assertEqual(self.result, ('/foo', 9))

    def test_checkout_or_export(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'checkout-or-export')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'checkout-or-export /foo')
        self.assertEqual(self.parse('checkout-or-export /foo r9'), '')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_unknown))
        self.assertRaises(svn_dav_log_parse.BadDepthError, self.parse,
                          'checkout-or-export /foo r9 depth=INVALID-DEPTH')
        self.assertRaises(svn_dav_log_parse.BadDepthError, self.parse,
                          'checkout-or-export /foo r9 depth=bork')
        self.assertEqual(self.parse('checkout-or-export /foo r9 depth=files .'),
                         ' .')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_files))

    def test_diff_or_merge_1path(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'diff-or-merge')
        self.assertEqual(self.parse('diff-or-merge /foo r9:10'), '')
        self.assertEqual(self.result, ('/foo', 9, 10,
                                       svn.core.svn_depth_unknown, False))
        self.assertEqual(self.parse('diff-or-merge /foo r9:10'
                                    ' ignore-ancestry ...'), ' ...')
        self.assertEqual(self.result, ('/foo', 9, 10,
                                       svn.core.svn_depth_unknown, True))
        self.assertEqual(self.parse('diff-or-merge /foo r9:10 depth=files'), '')
        self.assertEqual(self.result, ('/foo', 9, 10,
                                       svn.core.svn_depth_files, False))

    def test_diff_or_merge_2paths(self):
        self.assertEqual(self.parse('diff-or-merge /foo@9 /bar@10'), '')
        self.assertEqual(self.result, ('/foo', 9, '/bar', 10,
                                       svn.core.svn_depth_unknown, False))
        self.assertEqual(self.parse('diff-or-merge /foo@9 /bar@10'
                                    ' ignore-ancestry ...'), ' ...')
        self.assertEqual(self.result, ('/foo', 9, '/bar', 10,
                                       svn.core.svn_depth_unknown, True))
        self.assertEqual(self.parse('diff-or-merge /foo@9 /bar@10'
                                    ' depth=files ignore-ancestry'), '')
        self.assertEqual(self.result, ('/foo', 9, '/bar', 10,
                                       svn.core.svn_depth_files, True))

    def test_remote_status(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'remote-status')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'remote-status /foo')
        self.assertEqual(self.parse('remote-status /foo r9'), '')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_unknown))
        self.assertRaises(svn_dav_log_parse.BadDepthError, self.parse,
                          'remote-status /foo r9 depth=INVALID-DEPTH')
        self.assertRaises(svn_dav_log_parse.BadDepthError, self.parse,
                          'remote-status /foo r9 depth=bork')
        self.assertEqual(self.parse('remote-status /foo r9 depth=files .'),
                         ' .')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_files))

    def test_switch(self):
        self.assertEqual(self.parse('switch /foo@9 /bar@10 ...'), ' ...')
        self.assertEqual(self.result, ('/foo', 9, '/bar', 10,
                                       svn.core.svn_depth_unknown))
        self.assertEqual(self.parse('switch /foo@9 /bar@10'
                                    ' depth=files'), '')
        self.assertEqual(self.result, ('/foo', 9, '/bar', 10,
                                       svn.core.svn_depth_files))

    def test_update(self):
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'update')
        self.assertRaises(svn_dav_log_parse.Error,
                          self.parse, 'update /foo')
        self.assertEqual(self.parse('update /foo r9'), '')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_unknown,
                                       False))
        self.assertRaises(svn_dav_log_parse.BadDepthError, self.parse,
                          'update /foo r9 depth=INVALID-DEPTH')
        self.assertRaises(svn_dav_log_parse.BadDepthError, self.parse,
                          'update /foo r9 depth=bork')
        self.assertEqual(self.parse('update /foo r9 depth=files .'), ' .')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_files,
                                       False))
        self.assertEqual(self.parse('update /foo r9 send-copyfrom-args .'),
                         ' .')
        self.assertEqual(self.result, ('/foo', 9, svn.core.svn_depth_unknown,
                                       True))

if __name__ == '__main__':
    if len(sys.argv) == 1:
        # No arguments so run the unit tests.
        unittest.main()
        sys.stderr.write('unittest.main failed to exit\n')
        sys.exit(2)

    # Use the argument as the path to a log file to test against.

    # Define a class to reconstruct the SVN-ACTION string.
    class Test(svn_dav_log_parse.Parser):
        def handle_unknown(self, line):
            sys.stderr.write('unknown log line at %d\n' % (self.linenum,))
            sys.exit(2)

        def handle_commit(self, revision):
            self.action = 'commit r%d' % (revision,)

        def handle_list_dir(self, path, revision):
            self.action = 'list-dir %s r%d' % (path, revision)

        def handle_lock(self, path, steal):
            self.action = 'lock ' + path
            if steal:
                self.action += ' steal'

        def handle_prop_list(self, path, revision):
            self.action = 'prop-list %s@%d' % (path, revision)

        def handle_revprop_change(self, revision, revprop):
            self.action = 'revprop-change r%d %s' % (revision, revprop)

        def handle_revprop_list(self, revision):
            self.action = 'revprop-list r%d' % (revision,)

        def handle_unlock(self, path, break_lock):
            self.action = 'unlock ' + path
            if break_lock:
                self.action += ' break'

        # reports

        def handle_blame(self, path, left, right, include_merged_revisions):
            self.action = 'blame %s r%d:%d' % (path, left, right)
            if include_merged_revisions:
                self.action += ' include-merged-revisions'

        def handle_get_mergeinfo(self, paths, inheritance):
            self.action = ('get-mergeinfo (%s) %s'
                           % (' '.join(paths),
                              svn.core.svn_inheritance_to_word(inheritance)))

        def handle_log(self, paths, left, right, limit, discover_changed_paths,
                       strict, include_merged_revisions, revprops):
            self.action = 'log (%s) r%d:%d' % (' '.join(paths),
                                               left, right)
            if limit != 0:
                self.action += ' limit=%d' % (limit,)
            if discover_changed_paths:
                self.action += ' discover-changed-paths'
            if strict:
                self.action += ' strict'
            if include_merged_revisions:
                self.action += ' include-merged-revisions'
            if revprops is None:
                self.action += ' revprops=all'
            elif len(revprops) > 0:
                self.action += ' revprops=(%s)' % (' '.join(revprops),)

        def handle_replay(self, path, revision):
            self.action = 'replay %s r%d' % (path, revision)

        # the update report

        def maybe_depth(self, depth):
            if depth != svn.core.svn_depth_unknown:
                self.action += ' depth=%s' % (
                    svn.core.svn_depth_to_word(depth),)

        def handle_checkout_or_export(self, path, revision, depth):
            self.action = 'checkout-or-export %s r%d' % (path, revision)
            self.maybe_depth(depth)

        def handle_diff_or_merge_1path(self, path, left, right,
                                       depth, ignore_ancestry):
            self.action = 'diff-or-merge %s r%d:%d' % (path, left, right)
            self.maybe_depth(depth)
            if ignore_ancestry:
                self.action += ' ignore-ancestry'

        def handle_diff_or_merge_2paths(self, from_path, from_rev,
                                        to_path, to_rev,
                                        depth, ignore_ancestry):
            self.action = ('diff-or-merge %s@%d %s@%d'
                           % (from_path, from_rev, to_path, to_rev))
            self.maybe_depth(depth)
            if ignore_ancestry:
                self.action += ' ignore-ancestry'

        def handle_remote_status(self, path, revision, depth):
            self.action = 'remote-status %s r%d' % (path, revision)
            self.maybe_depth(depth)

        def handle_switch(self, from_path, from_rev,
                          to_path, to_rev, depth):
            self.action = ('switch %s@%d %s@%d'
                           % (from_path, from_rev, to_path, to_rev))
            self.maybe_depth(depth)

        def handle_update(self, path, revision, depth, send_copyfrom_args):
            self.action = 'update %s r%d' % (path, revision)
            self.maybe_depth(depth)
            if send_copyfrom_args:
                self.action += ' send-copyfrom-args'

    tmp = tempfile.mktemp()
    try:
        fp = open(tmp, 'w')
        parser = Test()
        parser.linenum = 0
        for line in open(sys.argv[1]):
            parser.linenum += 1
            # Find the SVN-ACTION string from the CustomLog format
            # davautocheck.sh uses.  If that changes, this will need
            # to as well.  Currently it's
            #   %t %u %{SVN-REPOS-NAME}e %{SVN-ACTION}e
            words = line.split()
            leading = ' '.join(words[:4])
            action = ' '.join(words[4:])
            # Parse the action and write the reconstructed action to
            # the temporary file.
            trailing = parser.parse(action)
            fp.write(leading + ' ' + parser.action + trailing + '\n')
        fp.close()
        # Check differences between original and reconstructed files
        # (should be identical).
        sys.exit(os.spawnlp(os.P_WAIT, 'diff', 'diff', '-u', sys.argv[1], tmp))
    finally:
        try:
            os.unlink(tmp)
        except Exception, e:
            sys.stderr.write('os.unlink(tmp): %s\n' % (e,))
