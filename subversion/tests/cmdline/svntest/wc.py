#
#  wc.py: functions for interacting with a Subversion working copy
#
#  Subversion is a tool for revision control.
#  See http://subversion.tigris.org for more information.
#
# ====================================================================
# Copyright (c) 2000-2006, 2008 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import os
import types
import sys
import re

import svntest


#
# 'status -v' output looks like this:
#
#      "%c%c%c%c%c%c%c %c   %6s   %6s %-12s %s\n"
#
# (Taken from 'print_status' in subversion/svn/status.c.)
#
# Here are the parameters.  The middle number or string in parens is the
# match.group(), followed by a brief description of the field:
#
#    - text status           (1)  (single letter)
#    - prop status           (1)  (single letter)
#    - wc-lockedness flag    (2)  (single letter: "L" or " ")
#    - copied flag           (3)  (single letter: "+" or " ")
#    - switched flag         (4)  (single letter: "S" or " ")
#    - repos lock status     (5)  (single letter: "K", "O", "B", "T", " ")
#    - tree conflict flag    (6)  (single letter: "C" or " ")
#
#    [one space]
#
#    - out-of-date flag      (7)  (single letter: "*" or " ")
#
#    [three spaces]
#
#    - working revision ('wc_rev') (either digits or "-" or " ")
#
#    [one space]
#
#    - last-changed revision      (either digits or "?" or " ")
#
#    [one space]
#
#    - last author                (optional string of non-whitespace
#                                  characters)
#
#    [spaces]
#
#    - path              ('path') (string of characters until newline)
#
# Working revision, last-changed revision, and last author are whitespace
# only if the item is missing.
#
_re_parse_status = re.compile('^([?!MACDRUG_ ][MACDRUG_ ])'
                              '([L ])'
                              '([+ ])'
                              '([S ])'
                              '([KOBT ])'
                              '([C ]) '
                              '([* ]) +'
                              '((?P<wc_rev>\d+|-|\?) +(\d|-|\?)+ +(\S+) +)?'
                              '(?P<path>.+)$')

_re_parse_skipped = re.compile("^Skipped.* '(.+)'\n")

_re_parse_summarize = re.compile("^([MAD ][M ])      (.+)\n")

_re_parse_checkout = re.compile('^([RMAGCUDE_ ][MAGCUDE_ ])'
                                '([B ])'
                                '([C ])\s+'
                                '(.+)')
_re_parse_co_skipped = re.compile('^(Restored|Skipped)\s+\'(.+)\'')
_re_parse_co_restored = re.compile('^(Restored)\s+\'(.+)\'')

# Lines typically have a verb followed by whitespace then a path.
_re_parse_commit = re.compile('^(\w+(  \(bin\))?)\s+(.+)')


class State:
  """Describes an existing or expected state of a working copy.

  The primary metaphor here is a dictionary of paths mapping to instances
  of StateItem, which describe each item in a working copy.

  Note: the paths should be *relative* to the root of the working copy.
  """

  def __init__(self, wc_dir, desc):
    "Create a State using the specified description."
    assert isinstance(desc, types.DictionaryType)

    self.wc_dir = wc_dir
    self.desc = desc      # dictionary: path -> StateItem

  def add(self, more_desc):
    "Add more state items into the State."
    assert isinstance(more_desc, types.DictionaryType)

    self.desc.update(more_desc)

  def add_state(self, parent, state):
    "Import state items from a State object, reparent the items to PARENT."
    assert isinstance(state, State)

    if parent and parent[-1] != '/':
      parent += '/'
    for path, item in state.desc.items():
      path = parent + path
      self.desc[path] = item

  def remove(self, *paths):
    "Remove a path from the state (the path must exist)."
    for path in paths:
      if sys.platform == 'win32':
        path = path.replace('\\', '/')
      del self.desc[path]

  def copy(self, new_root=None):
    """Make a deep copy of self.  If NEW_ROOT is not None, then set the
    copy's wc_dir NEW_ROOT instead of to self's wc_dir."""
    desc = { }
    for path, item in self.desc.items():
      desc[path] = item.copy()
    if new_root is None:
      new_root = self.wc_dir
    return State(new_root, desc)

  def tweak(self, *args, **kw):
    """Tweak the items' values.

    Each argument in ARGS is the path of a StateItem that already exists in
    this State. Each keyword argument in KW is a modifiable property of
    StateItem.

    The general form of this method is .tweak([paths...,] key=value...). If
    one or more paths are provided, then those items' values are
    modified.  If no paths are given, then all items are modified.
    """
    if args:
      for path in args:
        try:
          if sys.platform == 'win32':
            path = path.replace('\\', '/')
          path_ref = self.desc[path]
        except KeyError, e:
          e.args = ["Path '%s' not present in WC state descriptor" % path]
          raise
        path_ref.tweak(**kw)
    else:
      for item in self.desc.values():
        item.tweak(**kw)

  def tweak_some(self, filter, **kw):
    "Tweak the items for which the filter returns true."
    for path, item in self.desc.items():
      if list(filter(path, item)):
        item.tweak(**kw)

  def subtree(self, subtree_path):
    """Return a State object which is a deep copy of the sub-tree
    identified by SUBTREE_PATH (which is assumed to contain only one
    element rooted at the tree of this State object's WC_DIR)."""
    desc = { }
    for path, item in self.desc.items():
      path_elements = path.split("/")
      if len(path_elements) > 1 and path_elements[0] == subtree_path:
        desc["/".join(path_elements[1:])] = item.copy()
    return State(self.wc_dir, desc)

  def write_to_disk(self, target_dir):
    """Construct a directory structure on disk, matching our state.

    WARNING: any StateItem that does not have contents (.contents is None)
    is assumed to be a directory.
    """
    if not os.path.exists(target_dir):
      os.makedirs(target_dir)

    for path, item in self.desc.items():
      fullpath = os.path.join(target_dir, path)
      if item.contents is None:
        # a directory
        if not os.path.exists(fullpath):
          os.makedirs(fullpath)
      else:
        # a file

        # ensure its directory exists
        dirpath = os.path.dirname(fullpath)
        if not os.path.exists(dirpath):
          os.makedirs(dirpath)

        # write out the file contents now
        open(fullpath, 'wb').write(item.contents)

  def old_tree(self):
    "Return an old-style tree (for compatibility purposes)."
    nodelist = [ ]
    for path, item in self.desc.items():
      atts = { }
      if item.status is not None:
        atts['status'] = item.status
      if item.verb is not None:
        atts['verb'] = item.verb
      if item.wc_rev is not None:
        atts['wc_rev'] = item.wc_rev
      if item.locked is not None:
        atts['locked'] = item.locked
      if item.copied is not None:
        atts['copied'] = item.copied
      if item.switched is not None:
        atts['switched'] = item.switched
      if item.writelocked is not None:
        atts['writelocked'] = item.writelocked
      if item.treeconflict is not None:
        atts['treeconflict'] = item.treeconflict
      nodelist.append((os.path.normpath(os.path.join(self.wc_dir, path)),
                       item.contents,
                       item.props,
                       atts))

    tree = svntest.tree.build_generic_tree(nodelist)
    if 0:
      check = tree.as_state()
      if self != check:
        import pprint
        pprint.pprint(self.desc)
        pprint.pprint(check.desc)
        # STATE -> TREE -> STATE is lossy.
        # In many cases, TREE -> STATE -> TREE is not.
        # Even though our conversion from a TREE has lost some information, we
        # may be able to verify that our lesser-STATE produces the same TREE.
        svntest.tree.compare_trees('mismatch', tree, check.old_tree())

    return tree

  def __str__(self):
    return str(self.old_tree())

  def __eq__(self, other):
    return isinstance(other, State) and self.desc == other.desc

  def __ne__(self, other):
    return not self.__eq__(other)

  @classmethod
  def from_status(cls, lines):
    """Create a State object from 'svn status' output."""

    def not_space(value):
      if value and value != ' ':
        return value
      return None

    desc = { }
    for line in lines:
      if line.startswith('DBG:'):
        continue

      # Quit when we hit an externals status announcement.
      # ### someday we can fix the externals tests to expect the additional
      # ### flood of externals status data.
      if line.startswith('Performing'):
        break

      match = _re_parse_status.search(line)
      if not match or match.group(10) == '-':
        # ignore non-matching lines, or items that only exist on repos
        continue

      item = StateItem(status=match.group(1),
                       locked=not_space(match.group(2)),
                       copied=not_space(match.group(3)),
                       switched=not_space(match.group(4)),
                       writelocked=not_space(match.group(5)),
                       treeconflict=not_space(match.group(6)),
                       wc_rev=not_space(match.group('wc_rev')),
                       )
      desc[match.group('path')] = item

    return cls('', desc)

  @classmethod
  def from_skipped(cls, lines):
    """Create a State object from 'Skipped' lines."""

    desc = { }
    for line in lines:
      if line.startswith('DBG:'):
        continue

      match = _re_parse_skipped.search(line)
      if match:
        desc[match.group(1)] = StateItem()

    return cls('', desc)

  @classmethod
  def from_summarize(cls, lines):
    """Create a State object from 'svn diff --summarize' lines."""

    desc = { }
    for line in lines:
      if line.startswith('DBG:'):
        continue

      match = _re_parse_summarize.search(line)
      if match:
        desc[match.group(2)] = StateItem(status=match.group(1))

    return cls('', desc)

  @classmethod
  def from_checkout(cls, lines, include_skipped=True):
    """Create a State object from 'svn checkout' lines."""

    if include_skipped:
      re_extra = _re_parse_co_skipped
    else:
      re_extra = _re_parse_co_restored

    desc = { }
    for line in lines:
      if line.startswith('DBG:'):
        continue

      match = _re_parse_checkout.search(line)
      if match:
        if match.group(3) == 'C':
          treeconflict = 'C'
        else:
          treeconflict = None
        desc[match.group(4)] = StateItem(status=match.group(1),
                                         treeconflict=treeconflict)
      else:
        match = re_extra.search(line)
        if match:
          desc[match.group(2)] = StateItem(verb=match.group(1))

    return cls('', desc)

  @classmethod
  def from_commit(cls, lines):
    """Create a State object from 'svn commit' lines."""

    desc = { }
    for line in lines:
      if line.startswith('DBG:') or line.startswith('Transmitting'):
        continue

      match = _re_parse_commit.search(line)
      if match:
        desc[match.group(3)] = StateItem(verb=match.group(1))

    return cls('', desc)

  @classmethod
  def from_wc(cls, path, load_props=False, ignore_svn=True):
    """Create a State object from a working copy.

    Walks the tree at PATH, building a State based on the actual files
    and directories found. If LOAD_PROPS is True, then the properties
    will be loaded for all nodes (Very Expensive!). If IGNORE_SVN is
    True, then the .svn subdirectories will be excluded from the State.
    """
    # generally, the OS wants '.' rather than ''
    if not path:
      path = '.'

    desc = { }
    dot_svn = svntest.main.get_admin_name()

    def path_to_key(p, l=len(path)+1):
      if p == path:
        return ''
      assert p.startswith(path + os.sep), \
          "'%s' is not a prefix of '%s'" % (path + os.sep, p)
      return p[l:].replace(os.sep, '/')

    def _walker(baton, dirname, names):
      parent = path_to_key(dirname)
      if parent:
        parent += '/'
      if ignore_svn and (dot_svn in names):
        names.remove(dot_svn)
      for name in names:
        node = os.path.join(dirname, name)
        if os.path.isfile(node):
          contents = open(node, 'r').read()
        else:
          contents = None
        desc['%s%s' % (parent, name)] = StateItem(contents=contents)

    os.path.walk(path, _walker, None)

    if load_props:
      paths = [os.path.join(path, p.replace('/', os.sep)) for p in desc.keys()]
      paths.append(path)
      all_props = svntest.tree.get_props(paths)
      for node, props in all_props.items():
        if node == path:
          desc['.'] = StateItem(props=props)
        else:
          if path == '.':
            # 'svn proplist' strips './' from the paths. put it back on.
            node = './' + node
          desc[path_to_key(node)].props = props

    return cls('', desc)


class StateItem:
  """Describes an individual item within a working copy.

  Note that the location of this item is not specified. An external
  mechanism, such as the State class, will provide location information
  for each item.
  """

  def __init__(self, contents=None, props=None,
               status=None, verb=None, wc_rev=None,
               locked=None, copied=None, switched=None, writelocked=None,
               treeconflict=None):
    # provide an empty prop dict if it wasn't provided
    if props is None:
      props = { }

    ### keep/make these ints one day?
    if wc_rev is not None:
      wc_rev = str(wc_rev)

    # Any attribute can be None if not relevant, unless otherwise stated.

    # A string of content (if the node is a file).
    self.contents = contents
    # A dictionary mapping prop name to prop value; never None.
    self.props = props
    # A two-character string from the first two columns of 'svn status'.
    self.status = status
    # The action word such as 'Adding' printed by commands like 'svn update'.
    self.verb = verb
    # The base revision number of the node in the WC, as a string.
    self.wc_rev = wc_rev
    # For the following attributes, the value is the status character of that
    # field from 'svn status', except using value None instead of status ' '.
    self.locked = locked
    self.copied = copied
    self.switched = switched
    self.writelocked = writelocked
    # Value 'C' or ' ', or None as an expected status meaning 'do not check'.
    self.treeconflict = treeconflict

  def copy(self):
    "Make a deep copy of self."
    new = StateItem()
    vars(new).update(vars(self))
    new.props = self.props.copy()
    return new

  def tweak(self, **kw):
    for name, value in kw.items():
      ### refine the revision args (for now) to ensure they are strings
      if value is not None and name == 'wc_rev':
        value = str(value)
      setattr(self, name, value)

  def __eq__(self, other):
    if not isinstance(other, StateItem):
      return False
    v_self = vars(self)
    v_other = vars(other)
    if self.treeconflict is None:
      v_other = v_other.copy()
      v_other['treeconflict'] = None
    if other.treeconflict is None:
      v_self = v_self.copy()
      v_self['treeconflict'] = None
    return v_self == v_other

  def __ne__(self, other):
    return not self.__eq__(other)
