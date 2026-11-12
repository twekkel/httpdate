#define _DEFAULT_SOURCE

#include "log.h"
#include "ntpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NTP_EPOCH_OFFSET        2208988800ULL   // NTP-to-Unix epoch offset
#define NTP_PACKET_SIZE         48
#define NTP_PORT                123
#define NTP_LI_ALARM            3               // clock not synchronized
#define NTP_STRATUM             5
#define NTP_STRATUM_UNSYNC      16
#define NTP_MODE_CLIENT         3
#define NTP_MODE_SERVER         4
#define NTP_PRECISION           -6              // 2^-6  15.625 ms
#define NTP_REFID_HTTP          0x48545450      // "HTTP" clock reference
#define NTP_ROOT_DISPERSION     0x00002000      // 125 ms, when adjtimex fails
#define NTP_MAXERROR_USEC       500000L         // 500 ms cap on kernel maxerror
#define NTP_STATE_TTL           60              // s between adjtimex() reads
#define NTP_METRICS_FLUSH_TTL   10              // s between ntpd.prom writes

#if defined __NetBSD__ || defined __FreeBSD__
#define adjtimex ntp_adjtime
#endif

// NTP packet, as defined by RFC 5905
struct ntp_packet {
  uint8_t  li_vn_mode;
  uint8_t  stratum;
  uint8_t  poll;
  int8_t   precision;
  uint32_t root_delay;
  uint32_t root_dispersion;
  uint32_t ref_id;
  uint32_t ref_timestamp_sec;
  uint32_t ref_timestamp_frac;
  uint32_t orig_timestamp_sec;
  uint32_t orig_timestamp_frac;
  uint32_t rx_timestamp_sec;
  uint32_t rx_timestamp_frac;
  uint32_t tx_timestamp_sec;
  uint32_t tx_timestamp_frac;
} __attribute__((packed));

_Static_assert(sizeof(struct ntp_packet) == NTP_PACKET_SIZE, "NTP packet must be exactly 48 bytes");

static volatile sig_atomic_t running = 1;

#ifdef WITH_METRICS
static const char *ntpd_metrics_dir = NULL;
static unsigned long requests_ok;        // answered NTPv3/v4 client requests
static unsigned long requests_bad;       // ignored: wrong mode or version
static unsigned long requests_dropped;   // runt packets
static unsigned long metrics_flushed;    // counter total at the last file write
static time_t metrics_flushed_at;        // second of the last file write

// Write ntpd.prom atomically (tmp + rename), as expected by
// node_exporter's textfile collector
static void write_ntpd_metrics(void) {
  char path[PATH_MAX], tmp_path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/ntpd.prom", ntpd_metrics_dir);
  if (n < 0 || (size_t)n >= sizeof(path))
    return;
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  FILE *f = fopen(tmp_path, "w");
  if (!f) {
    print_log(1, "Failed to open %s: %s", tmp_path, strerror(errno));
    return;
  }

  fprintf(f,
    "# HELP httpdate_ntp_requests_total NTP requests received by the built-in server.\n"
    "# TYPE httpdate_ntp_requests_total counter\n"
    "httpdate_ntp_requests_total{result=\"ok\"} %lu\n"
    "httpdate_ntp_requests_total{result=\"bad\"} %lu\n"
    "httpdate_ntp_requests_total{result=\"dropped\"} %lu\n",
    requests_ok, requests_bad, requests_dropped);

  if (fclose(f) != 0)
    print_log(1, "Failed to write %s: %s", tmp_path, strerror(errno));
  else if (rename(tmp_path, path) < 0)
    print_log(1, "Failed to rename %s: %s", tmp_path, strerror(errno));
}

// Flush the counters at most once per NTP_METRICS_FLUSH_TTL seconds, so a
// request storm does not turn into a write storm
static void maybe_write_ntpd_metrics(void) {
  unsigned long total = requests_ok + requests_bad + requests_dropped;
  time_t now = time(NULL);
  if (total != metrics_flushed && now - metrics_flushed_at >= NTP_METRICS_FLUSH_TTL) {
    write_ntpd_metrics();
    metrics_flushed = total;
    metrics_flushed_at = now;
  }
}
#endif

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

static uint32_t nsec_to_ntp_frac(long nsec) {
  return (uint32_t)(((uint64_t)nsec << 32) / 1000000000ULL);
}

static void timespec_to_ntp(const struct timespec *ts, uint32_t *sec, uint32_t *frac) {
  uint64_t secs = (uint64_t)(int64_t)ts->tv_sec + NTP_EPOCH_OFFSET;
  *sec = (uint32_t)(secs & 0xFFFFFFFFULL);
  *frac = nsec_to_ntp_frac(ts->tv_nsec);
}

// Microseconds to the NTP short format (16.16 fixed point, seconds)
static uint32_t usec_to_ntp_short(long usec) {
  if (usec <= 0)
    return 0;
  uint64_t v = ((uint64_t)usec << 16) / 1000000ULL;
  return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static int bind_ntp_socket(int family, int *unsupported) {
  *unsupported = 0;

  int sockfd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
  if (sockfd < 0) {
    if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT)
      *unsupported = 1;
    else
      print_log(1, "socket: %s", strerror(errno));
    return -1;
  }

  struct sockaddr_storage addr;
  socklen_t addr_len;
  memset(&addr, 0, sizeof(addr));

  if (family == AF_INET6) {
    int off = 0;
    if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
      *unsupported = 1;
      close(sockfd);
      return -1;
    }
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&addr;
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(NTP_PORT);
    a6->sin6_addr = in6addr_any;
    addr_len = sizeof(*a6);
  } else {
    struct sockaddr_in *a4 = (struct sockaddr_in *)&addr;
    a4->sin_family = AF_INET;
    a4->sin_port = htons(NTP_PORT);
    a4->sin_addr.s_addr = htonl(INADDR_ANY);
    addr_len = sizeof(*a4);
  }

  // No SO_REUSEADDR/SO_REUSEPORT: a second httpdate -S instance must fail
  // with EADDRINUSE instead of silently stealing NTP packets
  if (bind(sockfd, (struct sockaddr *)&addr, addr_len) < 0) {
    print_log(1, "bind: %s", strerror(errno));
    close(sockfd);
    return -1;
  }

  return sockfd;
}

static int create_ntp_socket(void) {
  int unsupported;

  // Prefer one dual-stack IPv6 socket, which covers IPv4 as well
  int sockfd = bind_ntp_socket(AF_INET6, &unsupported);
  if (sockfd >= 0)
    return sockfd;
  if (!unsupported)
    return -1;

  // IPv6 not available: serve IPv4 only
  print_log(0, "IPv6 dual-stack unavailable, NTP server is IPv4 only");
  return bind_ntp_socket(AF_INET, &unsupported);
}

// Cache of the kernel clock state. adjtimex() is a syscall and the values
// only change when the parent process syncs the clock, so read them at
// most once per NTP_STATE_TTL seconds instead of once per NTP request.
static struct {
  uint32_t root_dispersion;
  int      unsync;
  time_t   when;              // 0 = no valid read yet
} clock_state = {0};

// now: current time in seconds, taken from the request's receive timestamp.
// The unsigned compare also expires the cache after a backwards clock step.
static void refresh_clock_state(time_t now) {
  if (clock_state.when && (uint64_t)(now - clock_state.when) < NTP_STATE_TTL)
    return;

  struct timex tmx = {0};
  if (adjtimex(&tmx) >= 0) {
    // The kernel inflates maxerror by ~500 us/s between the parent's
    // syncs; httpdate's corrected clock drifts only a few ppm, so cap
    // the advertised dispersion at a value we can actually guarantee.
    long maxerror = tmx.maxerror;
    if (maxerror > NTP_MAXERROR_USEC)
      maxerror = NTP_MAXERROR_USEC;

    clock_state.root_dispersion = usec_to_ntp_short(maxerror);
    clock_state.unsync = (tmx.status & STA_UNSYNC) != 0;
    clock_state.when = now;
  }
  // On failure keep the previous state; when == 0 selects the fixed fallback
}

// Answer one request, given its contents and receive timestamp
static void handle_ntp_request(
    int sockfd, const struct ntp_packet *request,
    const struct sockaddr_storage *client_addr, socklen_t client_len,
    const struct timespec *ts_recv) {
  unsigned int version = (request->li_vn_mode >> 3) & 0x07;
  unsigned int mode = request->li_vn_mode & 0x07;

  // Answer NTPv3 and v4 client requests; ignore everything else
  if (mode != NTP_MODE_CLIENT || (version != 3 && version != 4)) {
#ifdef WITH_METRICS
    requests_bad++;
#endif
    return;
  }

  // The kernel's view of the clock, maintained by httpdate: the maximum
  // error becomes the root dispersion, and an unsynchronized clock is
  // announced as such. Without adjtimex, fall back to fixed values.
  unsigned int li = 0;
  unsigned int stratum = NTP_STRATUM;
  uint32_t root_dispersion = NTP_ROOT_DISPERSION;

  refresh_clock_state(ts_recv->tv_sec);

  if (clock_state.when) {
    root_dispersion = clock_state.root_dispersion;
    if (clock_state.unsync) {
      li = NTP_LI_ALARM;
      stratum = NTP_STRATUM_UNSYNC;
    }
  }

  struct ntp_packet response;
  memset(&response, 0, sizeof(response));

  // LI from the clock state, VN echoes the client's version, Mode=4 (server)
  response.li_vn_mode = (uint8_t)((li << 6) | (version << 3) | NTP_MODE_SERVER);
  response.stratum = (uint8_t)stratum;
  response.poll = request->poll;
  response.precision = NTP_PRECISION;

  response.root_delay = htonl(0);
  response.root_dispersion = htonl(root_dispersion);
  response.ref_id = htonl(NTP_REFID_HTTP);

  // Receive timestamp also serves as the reference and transmit timestamps
  uint32_t ts_sec, ts_frac;
  timespec_to_ntp(ts_recv, &ts_sec, &ts_frac);

  response.ref_timestamp_sec = htonl(ts_sec);
  response.ref_timestamp_frac = htonl(ts_frac);

  response.orig_timestamp_sec = request->tx_timestamp_sec;
  response.orig_timestamp_frac = request->tx_timestamp_frac;

  response.rx_timestamp_sec = htonl(ts_sec);
  response.rx_timestamp_frac = htonl(ts_frac);

  response.tx_timestamp_sec = htonl(ts_sec);
  response.tx_timestamp_frac = htonl(ts_frac);

  ssize_t sent = sendto(sockfd, &response, sizeof(response), 0, (const struct sockaddr *)client_addr, client_len);
  if (sent < 0) {
    print_log(1, "sendto: %s", strerror(errno));
  } else if ((size_t)sent != sizeof(response)) {
    print_log(1, "sendto: incomplete NTP response");
  }
#ifdef WITH_METRICS
  if (sent == (ssize_t)sizeof(response))
    requests_ok++;
#endif
}

int run_ntp_server(int ready_fd, const char *metrics_dir) {
#ifndef WITH_METRICS
  (void)metrics_dir;
#else
  ntpd_metrics_dir = metrics_dir;
#endif

  int sockfd = create_ntp_socket();
  if (sockfd < 0) {
    print_log(1, "Failed to create NTP server socket");
    return -1;
  }

  // Shutdown signal is noticed within a second, not only on next NTP packet
  struct timeval recv_timeout = {1, 0};
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                 sizeof(recv_timeout)) < 0)
    print_log(1, "setsockopt SO_RCVTIMEO: %s", strerror(errno));

  if (ready_fd >= 0) {
    if (write(ready_fd, "1", 1) < 0)
      print_log(1, "write ready byte: %s", strerror(errno));
    close(ready_fd);
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);

  print_log(0, "NTP server started");

#ifdef WITH_METRICS
  // Counters start at zero; write the file so collectors see the server
  if (ntpd_metrics_dir) {
    write_ntpd_metrics();
    metrics_flushed_at = time(NULL);
  }
#endif

  while (running) {
    // Tolerate extension fields but ignore > 48-byte header
    struct ntp_packet request;
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

    ssize_t bytes_read = recvfrom(sockfd, &request, sizeof(request), 0, (struct sockaddr *)&client_addr, &client_len);
    int recv_errno = errno;

    if (bytes_read < 0) {
      if (recv_errno == EINTR)
        continue;
      // SO_RCVTIMEO expired: loop again to re-check running
      if (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK)
        continue;
      print_log(1, "recvfrom: %s", strerror(recv_errno));
      continue;
    }

    // Drop runt packets before spending a clock read on them. The length
    // check is a single compare, so it does not skew the receive timestamp.
    if ((size_t)bytes_read < sizeof(struct ntp_packet)) {
#ifdef WITH_METRICS
      requests_dropped++;
      maybe_write_ntpd_metrics();
#endif
      continue;
    }

    struct timespec ts_recv;
    if (clock_gettime(CLOCK_REALTIME, &ts_recv) < 0) {
      print_log(1, "clock_gettime: %s", strerror(errno));
      continue;
    }

    handle_ntp_request(sockfd, &request, &client_addr, client_len, &ts_recv);
#ifdef WITH_METRICS
    maybe_write_ntpd_metrics();
#endif
  }

  close(sockfd);
  print_log(0, "NTP server stopped");
  return 0;
}
