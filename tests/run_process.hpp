// Start a program, wait for it, and return its exit code: -1 if it could not
// start or was killed. Shared by the multi-process tests and benchmark.
//
// POSIX uses posix_spawn. macOS runs one system() at a time and holds a lock
// until the child exits, so programs started together from several threads
// would run one after another. Arguments are split on spaces, so no argument
// can contain one. The program path can.
#pragma once

#include <string>

#ifdef _WIN32
#  include <cstdlib>
#else
#  include <algorithm>
#  include <cerrno>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <vector>
extern char** environ;
#endif

inline int run_process(const std::string& program, const std::string& args) {
#ifdef _WIN32
    // cmd.exe strips the outer pair of quotes, so the whole command needs one
    // extra layer for paths that contain spaces.
    return std::system(("\"\"" + program + "\" " + args + "\"").c_str());
#else
    std::vector<std::string> words{program};
    for (size_t at = 0; at < args.size();) {
        const size_t end = std::min(args.find(' ', at), args.size());
        if (end > at) {
            words.push_back(args.substr(at, end - at));
        }
        at = end + 1;
    }
    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
