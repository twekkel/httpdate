#define _DEFAULT_SOURCE

#include "ntpd.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NTP_EPOCH_OFFSET        2208988800ULL    // 1900-01-01, seconds before 1970
#define NTP_PACKET_SIZE         48
#define NTP_PORT                123
#define NTP_STRATUM             5
#define NTP_VERSION             4
#define NTP_MODE_CLIENT         3
#define NTP_MODE_SERVER         4
#define NTP_POLL                6               // 2^6  64 s, conventional poll interval
#define NTP_PRECISION           -6              // 2^-6  ~15.6 us
#define NTP_REFID_LOCL          0x4C4F434C      // "LOCL", local clock reference

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

_Static_assert(sizeof(struct ntp_packet) == NTP_PACKET_SIZE,
               "NTP packet must be exactly 48 bytes");

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

static uint32_t nsec_to_ntp_frac(long nsec) {
  return (uint32_t)(((uint64_t)nsec << 32) / 1000000000ULL);
}

static void timespec_to_ntp(const struct timespec *ts, uint32_t *sec, uint32_t *frac) {
  *sec = (uint32_t)((uint64_t)ts->tv_sec + NTP_EPOCH_OFFSET);
  *frac = nsec_to_ntp_frac(ts->tv_nsec);
}

static int create_ntp_socket(void) {
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockfd < 0) {
    print_log(1, "socket: %s", strerror(errno));
    return -1;
  }

  int opt = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    print_log(1, "setsockopt SO_REUSEADDR: %s", strerror(errno));
    close(sockfd);
    return -1;
  }

#ifdef SO_REUSEPORT
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    print_log(1, "setsockopt SO_REUSEPORT: %s", strerror(errno));
    close(sockfd);
    return -1;
  }
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(NTP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    print_log(1, "bind: %s", strerror(errno));
    close(sockfd);
    return -1;
  }

  return sockfd;
}

/* The main loop receives the datagram exactly once and passes its contents
 * and receive timestamp here. */
static void handle_ntp_request(
    int sockfd, const struct ntp_packet *request,
    const struct sockaddr_in *client_addr, socklen_t client_len,
    const struct timespec *ts_recv) {
  unsigned int version = (request->li_vn_mode >> 3) & 0x07;
  unsigned int mode = request->li_vn_mode & 0x07;

  // Answer NTPv3 and v4 client requests; ignore everything else
  if (mode != NTP_MODE_CLIENT || (version != 3 && version != 4))
    return;

  struct timespec ts_xmit;
  if (clock_gettime(CLOCK_REALTIME, &ts_xmit) < 0) {
    print_log(1, "clock_gettime: %s", strerror(errno));
    return;
  }

  struct ntp_packet response;
  memset(&response, 0, sizeof(response));

  /* LI=0 (no warning), VN=4, Mode=4 (server). */
  response.li_vn_mode = (uint8_t)((NTP_VERSION << 3) | NTP_MODE_SERVER);
  response.stratum = NTP_STRATUM;
  response.poll = NTP_POLL;
  response.precision = NTP_PRECISION;

  /* A local-clock reference. These values do not imply synchronization
   * to an external source. */
  response.root_delay = htonl(0);
  response.root_dispersion = htonl(0);
  response.ref_id = htonl(NTP_REFID_LOCL);

  uint32_t ref_sec, ref_frac;
  timespec_to_ntp(&ts_xmit, &ref_sec, &ref_frac);
  response.ref_timestamp_sec = htonl(ref_sec);
  response.ref_timestamp_frac = htonl(ref_frac);

  /* Originate timestamp must be copied verbatim from the request. */
  response.orig_timestamp_sec = request->tx_timestamp_sec;
  response.orig_timestamp_frac = request->tx_timestamp_frac;

  uint32_t rx_sec, rx_frac;
  timespec_to_ntp(ts_recv, &rx_sec, &rx_frac);
  response.rx_timestamp_sec = htonl(rx_sec);
  response.rx_timestamp_frac = htonl(rx_frac);

  uint32_t tx_sec, tx_frac;
  timespec_to_ntp(&ts_xmit, &tx_sec, &tx_frac);
  response.tx_timestamp_sec = htonl(tx_sec);
  response.tx_timestamp_frac = htonl(tx_frac);

  ssize_t sent = sendto(sockfd, &response, sizeof(response), 0, (const struct sockaddr *)client_addr, client_len);
  if (sent < 0) {
    print_log(1, "sendto: %s", strerror(errno));
  } else if ((size_t)sent != sizeof(response)) {
    print_log(1, "sendto: incomplete NTP response");
  }
}

int run_ntp_server(int foreground, int ready_fd) {
  running = 1;

  int sockfd = create_ntp_socket();
  if (sockfd < 0) {
    print_log(1, "Failed to create NTP server socket");
    return -1;
  }

  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0) {
      print_log(1, "fork: %s", strerror(errno));
      close(sockfd);
      return -1;
    }
    if (pid > 0) {
      close(sockfd);
      return 0;
    }

    if (setsid() < 0) {
      print_log(1, "setsid: %s", strerror(errno));
      close(sockfd);
      _exit(EXIT_FAILURE);
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      (void)dup2(fd, STDIN_FILENO);
      (void)dup2(fd, STDOUT_FILENO);
      (void)dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO)
        close(fd);
    }
  }

  // Report readiness to the parent process, so it can fail on startup errors
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

  print_log(0, "NTP server started on UDP port %d, stratum %u",
            NTP_PORT, NTP_STRATUM);

  while (running) {
    // Leave room for NTP extension fields; only the first 48 bytes are
    // the fixed header used by this minimal server.
    uint8_t buf[512];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    ssize_t bytes_read = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &client_len);
    if (bytes_read < 0) {
      if (errno == EINTR)
        continue;
      if (!running)
        break;
      print_log(1, "recvfrom: %s", strerror(errno));
      continue;
    }

    struct timespec ts_recv;
    if (clock_gettime(CLOCK_REALTIME, &ts_recv) < 0) {
      print_log(1, "clock_gettime: %s", strerror(errno));
      continue;
    }

    if ((size_t)bytes_read < sizeof(struct ntp_packet))
      continue;

    struct ntp_packet request;
    memcpy(&request, buf, sizeof(request));

    handle_ntp_request(sockfd, &request, &client_addr, client_len, &ts_recv);
  }

  close(sockfd);
  print_log(0, "NTP server stopped");
  return 0;
}

