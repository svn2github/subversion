#!/usr/bin/env python
#
#  svn_output.py:  module to parse various kinds of line-oriented output
#                  from the svn command-line client
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2001 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################
#
# Usage:  call the public routine svn_output.compare_line_lists(),
#         passing the list of lines that your test expects,
#                 the list of lines actually returned by svn,
#                 and a regexp for the kind of line matching you want.
#
# In theory, line order shouldn't matter: we're comparing *unordered*
# sets of lines.  Within a line, you must choose a regexp that
# stresses match-groups with \s+ whitespace between.  The match-groups
# are the things compared to determine if two lines match -- insuring
# that whitespace doesn't matter.

# Useful regexp for checkout/update:  r"^(..)\s+(.+)"  ==> '_U /foo/bar'
# Useful regexp for commit/import:    r"^(.+)\s+(.+)"  ==> "Changing /foo/bar'
# Useful regexp for status:           r"^(..)\s+(\d+)\s+\(.+\)\s+(.+)"
#                                        'MM     13      ( 24)   /foo/bar'
#
#####################################################################

import re  # the regexp library

# Helper for compare_line_lists()
def compare_lines(line1, line2, re_machine):
  """Use precompiled regexp in RE_MACHINE to test if LINE1 and LINE2 are
  the same.  (See docstring for compare_line_lists())"""

  match1 = re_machine.search(line1)
  match2 = re_machine.search(line2)

  if match1 is None or match2 is None:
    return 1  # failure: at least one line didn't match regexp
  if len(match1.groups()) != len(match2.groups()):
    return 1  # failure: the lines didn't have the same no. of group matches

  for i in range(1, len(match1.groups()) + 1):
    if match1.group(i) != match2.group(i):
      return 1 # failure:  a pair of match groups is different

  # if we get here, then all groups matched.
  return 0  # success



# Main exported func
def compare_line_lists(expected_lines, actual_lines, regexp):
  """Compare two lists of lines (ignoring orderings), and return 0 if
   they are the same or 1 if they are different.

   Specifically, matches will be made between each line in
   EXPECTED_LINES and those in ACTUAL_LINES using REGEXP.  If a line
   has no match, or if one of the lists has 'leftover' lines at the
   end, then the comparison will return 1.

   REGEXP should contain some non-zero number of match groups
   (presumably separated by arbitrary whitespace (\s+)).  A 'match'
   between lines will compare the first pair of match groups, then the
   second, and so on.  If all pairs match, then the lines themselves
   are said to match."""

  re_machine = re.compile(regexp)
  

