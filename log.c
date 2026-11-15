#define _DEFAULT_SOURCE

#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define PRINTBUFFERSIZE 1024
#define METRICSBUFFERSIZE 4096

static enum log_dest log_dest = LOG_DEST_CONSOLE;

void set_log_dest(enum log_dest dest) {
  log_dest = dest;
}

void print_log(int is_error, const char *format, ...) {
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
  }
}

#ifdef WITH_METRICS
// Shared by httpdate.prom and ntpd.prom: the caller only supplies the
// exposition text, the atomic tmp + rename dance lives here so both
// writers stay one call
void write_metrics_file(const char *dir, const char *name, const char *format, ...) {
  if (!dir)
    return;

  char path[PATH_MAX], tmp_path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= sizeof(path)) {
    print_log(1, "Metrics path too long: %s/%s", dir, name);
    return;
  }
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  va_list args;
  char buf[METRICSBUFFERSIZE];
  va_start(args, format);
  (void) vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  FILE *f = fopen(tmp_path, "w");
  if (!f) {
    print_log(1, "Failed to open %s: %s", tmp_path, strerror(errno));
    return;
  }

  fputs(buf, f);
  if (fclose(f) != 0)
    print_log(1, "Failed to write %s: %s", tmp_path, strerror(errno));
  else if (rename(tmp_path, path) < 0)
    print_log(1, "Failed to rename %s: %s", tmp_path, strerror(errno));
}
#endif
