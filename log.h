#ifndef LOG_H
#define LOG_H

enum log_dest {
  LOG_DEST_CONSOLE,
  LOG_DEST_SYSLOG
};

// Set the destination for subsequent print_log() calls
void set_log_dest(enum log_dest dest);

// Print a log line to the configured destination.
// is_error selects the error stream / severity.
void print_log(int is_error, const char *format, ...);

#endif
