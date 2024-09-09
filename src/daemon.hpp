#ifndef DAEMON
#define DAEMON

#include <fcntl.h>
#include <unistd.h>

//daemon returns true only in the daemon process when no error occurs
inline bool daemon(char const *logFile) {
    if (auto pid = fork(); pid < 0) return false;                        // an error occured
    else if (pid > 0) return false;                                      // return false from parent process
    if (setsid() < 0) return false;                                      // an error occured
    if (auto pid = fork(); pid < 0) return false;                        // an error occured
    else if (pid > 0) return false;                                      // return false from parent process
    for (int f = 0, max = sysconf(_SC_OPEN_MAX); f <= max; ++f) close(f);// close all open file descriptors
    if (not logFile) return true;
    //open the log file and redirect stdout to it
    auto const logFd = open(logFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFd == -1) return false;
    if (dup2(logFd, STDOUT_FILENO) == -1) return false;
    close(logFd);
    return true;
}

#endif
