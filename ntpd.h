#ifndef NTPD_H
#define NTPD_H

// Run the NTP server in the foreground; the caller daemonizes if needed.
// After the socket is bound, one byte is written to ready_fd so the
// parent process can detect startup failures. Pass a negative fd to disable.
int run_ntp_server(int ready_fd);

#endif
