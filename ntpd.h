#ifndef NTPD_H
#define NTPD_H

#include <signal.h>
#include <stdint.h>

// Run NTP server
// If foreground is non-zero, runs in foreground; otherwise daemonizes
static int run_ntp_server(int foreground);

#endif // NTPD_H
