package SVN::Fs;
use SVN::Base qw(Fs svn_fs_);

package _p_svn_fs_t;

our @methods = qw/youngest_rev revision_root revision_prop revision_proplist
		  change_rev_prop list_transactions open_txn begin_txn
		  get_uuid set_uuid/;

for (@methods) {
    *{$_} = *{"SVN::Fs::$_"};
}

package _p_svn_fs_root_t;

our @methods = qw/apply_textdelta apply_text change_node_prop
		 check_path close_root copied_from copy
		 dir_entries delete file_contents
		 file_length file_md5_checksum is_dir is_file
		 is_revision_root is_txn_root make_dir make_file
		 node_created_rev node_history node_id node_prop
		 node_proplist paths_changed revision_link
		 revision_root_revision/;

*fs = *{"SVN::Fs::root_fs"};

for (@methods) {
    *{$_} = *{"SVN::Fs::$_"};
}

package _p_svn_fs_history_t;
use SVN::Base qw/Fs svn_fs_history_/;

package _p_svn_fs_txn_t;
use SVN::Base qw/Fs svn_fs_txn_/;

*commit = *SVN::Fs::commit_txn;
*abort = *SVN::Fs::abort_txn;
*change_prop = *SVN::Fs::change_txn_prop;

package _p_svn_fs_dirent_t;
use SVN::Base qw(Fs svn_fs_dirent_t_);

package _p_svn_fs_path_change_t;
use SVN::Base qw(Fs svn_fs_path_change_t_);

package SVN::Fs::PathChange;
use SVN::Base qw(Fs svn_fs_path_change_);

1;
