#ifndef NTPD_H
#define NTPD_H

#include <signal.h>
#include <stdint.h>

// Run NTP server with specified stratum
// If foreground is non-zero, runs in foreground; otherwise daemonizes
void run_ntp_server(uint8_t stratum, int foreground);

#endif // NTPD_H
