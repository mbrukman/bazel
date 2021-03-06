// Copyright 2015 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>  // PATH_MAX
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/errors.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/file_platform.h"
#include "src/main/cpp/util/md5.h"

namespace blaze {

using blaze_util::die;
using blaze_util::pdie;

using std::string;
using std::vector;

string GetProcessIdAsString() {
  return ToString(getpid());
}

void ExecuteProgram(const string &exe, const vector<string> &args_vector) {
  if (VerboseLogging()) {
    string dbg;
    for (const auto &s : args_vector) {
      dbg.append(s);
      dbg.append(" ");
    }

    string cwd = blaze_util::GetCwd();
    fprintf(stderr, "Invoking binary %s in %s:\n  %s\n", exe.c_str(),
            cwd.c_str(), dbg.c_str());
  }

  // Copy to a char* array for execv:
  int n = args_vector.size();
  const char **argv = new const char *[n + 1];
  for (int i = 0; i < n; ++i) {
    argv[i] = args_vector[i].c_str();
  }
  argv[n] = NULL;

  execv(exe.c_str(), const_cast<char **>(argv));
}

std::string ConvertPath(const std::string &path) { return path; }

std::string ConvertPathList(const std::string& path_list) { return path_list; }

std::string ListSeparator() { return ":"; }

bool SymlinkDirectories(const string &target, const string &link) {
  return symlink(target.c_str(), link.c_str()) == 0;
}

// Causes the current process to become a daemon (i.e. a child of
// init, detached from the terminal, in its own session.)  We don't
// change cwd, though.
static void Daemonize(const string& daemon_output) {
  // Don't call die() or exit() in this function; we're already in a
  // child process so it won't work as expected.  Just don't do
  // anything that can possibly fail. :)

  signal(SIGHUP, SIG_IGN);
  if (fork() > 0) {
    // This second fork is required iff there's any chance cmd will
    // open an specific tty explicitly, e.g., open("/dev/tty23"). If
    // not, this fork can be removed.
    _exit(blaze_exit_code::SUCCESS);
  }

  setsid();

  close(0);
  close(1);
  close(2);

  open("/dev/null", O_RDONLY);  // stdin
  // stdout:
  if (open(daemon_output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) == -1) {
    // In a daemon, no-one can hear you scream.
    open("/dev/null", O_WRONLY);
  }
  (void) dup(STDOUT_FILENO);  // stderr (2>&1)
}

class PipeBlazeServerStartup : public BlazeServerStartup {
 public:
  PipeBlazeServerStartup(int pipe_fd);
  virtual ~PipeBlazeServerStartup();
  virtual bool IsStillAlive();

 private:
  int pipe_fd;
};

PipeBlazeServerStartup::PipeBlazeServerStartup(int pipe_fd) {
  this->pipe_fd = pipe_fd;
  if (fcntl(pipe_fd, F_SETFL, O_NONBLOCK | fcntl(pipe_fd, F_GETFL))) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
         "Failed: fcntl to enable O_NONBLOCK on pipe");
  }
}

PipeBlazeServerStartup::~PipeBlazeServerStartup() {
  close(pipe_fd);
}

bool PipeBlazeServerStartup::IsStillAlive() {
  char c;
  return read(this->pipe_fd, &c, 1) == -1 && errno == EAGAIN;
}

void WriteSystemSpecificProcessIdentifier(const string& server_dir);

void ExecuteDaemon(const string& exe,
                   const std::vector<string>& args_vector,
                   const string& daemon_output, const string& server_dir,
                   BlazeServerStartup** server_startup) {
  int fds[2];
  if (pipe(fds)) {
    pdie(blaze_exit_code::INTERNAL_ERROR, "pipe creation failed");
  }
  int child = fork();
  if (child == -1) {
    pdie(blaze_exit_code::INTERNAL_ERROR, "fork() failed");
  } else if (child > 0) {  // we're the parent
    close(fds[1]);  // parent keeps only the reading side
    int unused_status;
    waitpid(child, &unused_status, 0);  // child double-forks
    *server_startup = new PipeBlazeServerStartup(fds[0]);
    return;
  } else {
    close(fds[0]);  // child keeps only the writing side
  }

  Daemonize(daemon_output);
  string pid_string = GetProcessIdAsString();
  string pid_file = blaze_util::JoinPath(server_dir, kServerPidFile);
  string pid_symlink_file = blaze_util::JoinPath(server_dir, kServerPidSymlink);

  if (!WriteFile(pid_string, pid_file)) {
    // The exit code does not matter because we are already in the daemonized
    // server. The output of this operation will end up in jvm.out .
    pdie(0, "Cannot write PID file");
  }

  UnlinkPath(pid_symlink_file.c_str());
  if (symlink(pid_string.c_str(), pid_symlink_file.c_str()) < 0) {
    pdie(0, "Cannot write PID symlink");
  }

  WriteSystemSpecificProcessIdentifier(server_dir);

  ExecuteProgram(exe, args_vector);
  pdie(0, "Cannot execute %s", exe.c_str());
}

string RunProgram(const string& exe, const std::vector<string>& args_vector) {
  int fds[2];
  if (pipe(fds)) {
    pdie(blaze_exit_code::INTERNAL_ERROR, "pipe creation failed");
  }

  int child = fork();
  if (child == -1) {
    pdie(blaze_exit_code::INTERNAL_ERROR, "fork() failed");
  } else if (child > 0) {  // we're the parent
    close(fds[1]);         // parent keeps only the reading side
    string result;
    if (!ReadFileDescriptor(fds[0], &result)) {
      pdie(blaze_exit_code::INTERNAL_ERROR, "Cannot read subprocess output");
    }

    return result;
  } else {          // We're the child
    close(fds[0]);  // child keeps only the writing side
    // Redirect output to the writing side of the dup.
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    // Execute the binary
    ExecuteProgram(exe, args_vector);
    pdie(blaze_exit_code::INTERNAL_ERROR, "Failed to run %s", exe.c_str());
  }
  return string("");  //  We cannot reach here, just placate the compiler.
}

bool ReadDirectorySymlink(const string &name, string* result) {
  char buf[PATH_MAX + 1];
  int len = readlink(name.c_str(), buf, PATH_MAX);
  if (len < 0) {
    return false;
  }

  buf[len] = 0;
  *result = buf;
  return true;
}

bool CompareAbsolutePaths(const string& a, const string& b) {
  return a == b;
}

string GetHashedBaseDir(const string& root, const string& hashable) {
  unsigned char buf[blaze_util::Md5Digest::kDigestLength];
  blaze_util::Md5Digest digest;
  digest.Update(hashable.data(), hashable.size());
  digest.Finish(buf);
  return root + "/" + digest.String();
}

void CreateSecureOutputRoot(const string& path) {
  const char* root = path.c_str();
  struct stat fileinfo = {};

  if (!MakeDirectories(root, 0755)) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "mkdir('%s')", root);
  }

  // The path already exists.
  // Check ownership and mode, and verify that it is a directory.

  if (lstat(root, &fileinfo) < 0) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "lstat('%s')", root);
  }

  if (fileinfo.st_uid != geteuid()) {
    die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "'%s' is not owned by me",
        root);
  }

  if ((fileinfo.st_mode & 022) != 0) {
    int new_mode = fileinfo.st_mode & (~022);
    if (chmod(root, new_mode) < 0) {
      die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR,
          "'%s' has mode %o, chmod to %o failed", root,
          fileinfo.st_mode & 07777, new_mode);
    }
  }

  if (stat(root, &fileinfo) < 0) {
    pdie(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "stat('%s')", root);
  }

  if (!S_ISDIR(fileinfo.st_mode)) {
    die(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR, "'%s' is not a directory",
        root);
  }

  ExcludePathFromBackup(root);
}

// Runs "stat" on `path`. Returns -1 and sets errno if stat fails or
// `path` isn't a directory. If check_perms is true, this will also
// make sure that `path` is owned by the current user and has `mode`
// permissions (observing the umask). It attempts to run chmod to
// correct the mode if necessary. If `path` is a symlink, this will
// check ownership of the link, not the underlying directory.
static bool GetDirectoryStat(const string& path, mode_t mode,
                             bool check_perms) {
  struct stat filestat = {};
  if (stat(path.c_str(), &filestat) == -1) {
    return false;
  }

  if (!S_ISDIR(filestat.st_mode)) {
    errno = ENOTDIR;
    return false;
  }

  if (check_perms) {
    // If this is a symlink, run checks on the link. (If we did lstat above
    // then it would return false for ISDIR).
    struct stat linkstat = {};
    if (lstat(path.c_str(), &linkstat) != 0) {
      return false;
    }
    if (linkstat.st_uid != geteuid()) {
      // The directory isn't owned by me.
      errno = EACCES;
      return false;
    }

    mode_t mask = umask(022);
    umask(mask);
    mode = (mode & ~mask);
    if ((filestat.st_mode & 0777) != mode
        && chmod(path.c_str(), mode) == -1) {
      // errno set by chmod.
      return false;
    }
  }
  return true;
}

static bool MakeDirectories(const string& path, mode_t mode, bool childmost) {
  if (path.empty() || path == "/") {
    errno = EACCES;
    return false;
  }

  bool stat_succeeded = GetDirectoryStat(path, mode, childmost);
  if (stat_succeeded) {
    return true;
  }

  if (errno == ENOENT) {
    // Path does not exist, attempt to create its parents, then it.
    string parent = blaze_util::Dirname(path);
    if (!MakeDirectories(parent, mode, false)) {
      // errno set by stat.
      return false;
    }

    if (mkdir(path.c_str(), mode) == -1) {
      if (errno == EEXIST) {
        if (childmost) {
          // If there are multiple bazel calls at the same time then the
          // directory could be created between the MakeDirectories and mkdir
          // calls. This is okay, but we still have to check the permissions.
          return GetDirectoryStat(path, mode, childmost);
        } else {
          // If this isn't the childmost directory, we don't care what the
          // permissions were. If it's not even a directory then that error will
          // get caught when we attempt to create the next directory down the
          // chain.
          return true;
        }
      }
      // errno set by mkdir.
      return false;
    }
    return true;
  }

  return stat_succeeded;
}

// mkdir -p path. Returns 0 if the path was created or already exists and could
// be chmod-ed to exactly the given permissions. If final part of the path is a
// symlink, this ensures that the destination of the symlink has the desired
// permissions. It also checks that the directory or symlink is owned by us.
// On failure, this returns -1 and sets errno.
bool MakeDirectories(const string& path, unsigned int mode) {
  return MakeDirectories(path, mode, true);
}

string GetEnv(const string& name) {
  char* result = getenv(name.c_str());
  return result != NULL ? string(result) : "";
}

void SetEnv(const string& name, const string& value) {
  setenv(name.c_str(), value.c_str(), 1);
}

void UnsetEnv(const string& name) {
  unsetenv(name.c_str());
}

}   // namespace blaze.
