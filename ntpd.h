successfully downloaded text file (SHA: b799ad124cc828f5eb6eee4979644cd5b6318694)#ifndef NTPD_H
#define NTPD_H

#include <signal.h>
#include <stdint.h>

// Run NTP server
// If foreground is non-zero, runs in foreground; otherwise daemonizes
int run_ntp_server(int foreground);

#endif // NTPD_H
