successfully downloaded text file (SHA: 22553c7c7457e5357b5c219415079b6bad38a808)#ifndef NTPD_H
#define NTPD_H

#include <signal.h>
#include <stdint.h>

// Run NTP server
// If foreground is non-zero, runs in foreground; otherwise daemonizes
// After the socket is bound, one byte is written to ready_fd so the
// parent process can detect startup failures. Pass a negative fd to disable.
int run_ntp_server(int foreground, int ready_fd);

#endif // NTPD_H
