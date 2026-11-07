successfully downloaded text file (SHA: 712f1855c2c29a9401a2eee628fc5f5087e01eee)#ifndef NTPD_H
#define NTPD_H

#include <stdint.h>

// Run the NTP server in the foreground; the caller daemonizes if needed.
// After the socket is bound, one byte is written to ready_fd so the
// parent process can detect startup failures. Pass a negative fd to disable.
int run_ntp_server(int ready_fd);

#endif

