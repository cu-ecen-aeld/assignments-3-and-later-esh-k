#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdio.h>

extern int errno;

int main(int argc, char *argv[]) {
  openlog("ased-finder-app", 0, LOG_USER);
  if (argc < 3) {
    printf("incorrect number of arguments");
    syslog(LOG_ERR, "incorrect number of arguments");
    return 1;
  }
  const char *file_name = argv[1];
  int fd = open(file_name, O_WRONLY | O_CREAT, 0777);
  if (fd == -1) {
    printf("could not open file %s", file_name);
    syslog(LOG_ERR, "could not open file %s", file_name);
    return 1;
  }
  const char *str = argv[2];
  ssize_t len = strlen(str);
  syslog(LOG_DEBUG, "Writing %s to %s", file_name, str);
  ssize_t count = write(fd, str, len);
  if (count == -1) {
    printf("Failed to write with errcode: %d", errno);
    syslog(LOG_ERR, "Failed to write with errcode: %d", errno);
    return 1;
  }
  int err = close(fd);
  if (err == -1) {
    printf("could not close file %s", file_name);
    syslog(LOG_ERR, "could not close file %s", file_name);
    return 1;
  }
  closelog();
  return 0;
}
