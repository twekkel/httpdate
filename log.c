#define _DEFAULT_SOURCE

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

#define PRINTBUFFERSIZE 1024

static enum log_dest log_dest = LOG_DEST_CONSOLE;

void set_log_dest(enum log_dest dest) {
  log_dest = dest;
}

void print_log(int is_error, char *format, ...) {
  va_list args;
  char buf[PRINTBUFFERSIZE];

  va_start(args, format);
  (void) vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  switch(log_dest) {
    case LOG_DEST_CONSOLE:
      fprintf(is_error ? stderr : stdout, "%s\n", buf);
      fflush(is_error ? stderr : stdout);
      break;
    case LOG_DEST_SYSLOG:
      syslog(is_error ? LOG_WARNING : LOG_INFO, "%s", buf);
      break;
    default:
      fprintf(stderr, "%s\n", "Invalid logmode, aborting");
      abort();
  }
}
