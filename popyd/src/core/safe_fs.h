// Filesystem operations confined to a watch root.
//
// Defends against:
//   - symlink traversal (we use O_NOFOLLOW everywhere we open within a root)
//   - TOCTOU between resolve and open (we operate via dir_fd + relative path)
//   - paths that escape the root (verified after canonicalize)
//
// Every public function takes a "root" and a path that must, after lexical
// normalization, resolve under that root. Anything else throws.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace popy::safe_fs {

// Open `root` as a directory fd. Caller owns the fd; close with ::close().
int open_root(const std::filesystem::path& root);

// Open a regular file under `root_fd` for writing, O_CREAT | O_EXCL.
// `rel` is interpreted relative to root_fd; ".." is rejected.
// Mode 0600 by default — caller fchmod()s once writing is complete.
int create_at(int root_fd, std::string_view rel, mode_t mode = 0600);

// Open a regular file under `root_fd` for reading. Refuses symlinks
// (O_NOFOLLOW) and directories.
int open_read_at(int root_fd, std::string_view rel);

// mkdir -p relative to root_fd. Each component is created with mode 0700,
// O_NOFOLLOW. Throws if any component is a symlink.
void mkdirs_at(int root_fd, std::string_view rel);

// Atomic rename within the same root.
void rename_at(int root_fd, std::string_view from_rel, std::string_view to_rel);

// Whether `rel` is a "safe" relative path: non-empty, no leading '/', no
// ".." component, no embedded NUL.
bool is_safe_rel(std::string_view rel);

}  // namespace popy::safe_fs
