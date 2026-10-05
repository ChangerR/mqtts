// Test-only LD_PRELOAD injector. Never linked into or shipped with the broker.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int selected(int fd, const char* mode)
{
  const char* root = getenv("MQTTS_JOURNAL_FAULT_CONTROL");
  if (!root)
    return 0;
  char marker[PATH_MAX], pattern[128] = {0}, link[64], path[PATH_MAX];
  snprintf(marker, sizeof(marker), "%s/%s", root, mode);
  int control = (int)syscall(SYS_openat, AT_FDCWD, marker, O_RDONLY | O_CLOEXEC, 0);
  if (control < 0)
    return 0;
  ssize_t length = syscall(SYS_read, control, pattern, sizeof(pattern) - 1);
  syscall(SYS_close, control);
  if (length <= 0)
    return 0;
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  length = readlink(link, path, sizeof(path) - 1);
  if (length < 0)
    return 0;
  path[length] = 0;
  return strstr(path, pattern) != NULL;
}
ssize_t write(int fd, const void* data, size_t bytes)
{
  if (selected(fd, "write")) {
    errno = EIO;
    return -1;
  }
  return syscall(SYS_write, fd, data, bytes);
}
int fdatasync(int fd)
{
  while (selected(fd, "delay")) {
    struct timespec delay = {0, 1000000};
    syscall(SYS_nanosleep, &delay, NULL);
  }
  if (selected(fd, "sync")) {
    errno = EIO;
    return -1;
  }
  return (int)syscall(SYS_fdatasync, fd);
}
