#define _DEFAULT_SOURCE

#include "ntpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NTP_PORT 123
#define NTP_VERSION 4
#define NTP_PACKET_SIZE 48
#define NTP_EPOCH_OFFSET 2208988800ULL  // 1900 - 1970 in seconds

// NTP packet structure (RFC 5905)
struct ntp_packet {
  uint8_t li_vn_mode;      // Leap indicator (2 bits), version (3 bits), mode (3 bits)
  uint8_t stratum;         // Stratum level
  uint8_t poll;            // Poll interval
  int8_t precision;        // Precision (signed)
  uint32_t root_delay;     // Root delay (network byte order)
  uint32_t root_dispersion; // Root dispersion (network byte order)
  uint32_t ref_id;         // Reference ID (network byte order)
  uint32_t ref_timestamp_sec;   // Reference timestamp - seconds (network byte order)
  uint32_t ref_timestamp_frac;  // Reference timestamp - fraction (network byte order)
  uint32_t orig_timestamp_sec;   // Originate timestamp - seconds (network byte order)
  uint32_t orig_timestamp_frac;  // Originate timestamp - fraction (network byte order)
  uint32_t rx_timestamp_sec;     // Receive timestamp - seconds (network byte order)
  uint32_t rx_timestamp_frac;    // Receive timestamp - fraction (network byte order)
  uint32_t tx_timestamp_sec;     // Transmit timestamp - seconds (network byte order)
  uint32_t tx_timestamp_frac;    // Transmit timestamp - fraction (network byte order)
} __attribute__((packed));

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

// Convert fractional seconds (useconds) to NTP fraction (2^32)
static uint32_t usec_to_ntp_frac(uint32_t usec) {
  // (usec / 1000000) * 2^32 = (usec * 2^32) / 1000000
  // Use fixed-point arithmetic to avoid floating point
  // 2^32 / 1000000 = 4294.967296
  // So: frac = usec * 4294 + (usec * 0.967296) approximately
  // Using integer arithmetic: (usec * 4294967296ULL) / 1000000ULL
  return (uint32_t)(((uint64_t)usec * 4294967296ULL) / 1000000ULL);
}

// Get current NTP timestamp in host byte order
static void get_current_ntp_timestamp(uint32_t *sec, uint32_t *frac) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  *sec = (uint32_t)(tv.tv_sec + NTP_EPOCH_OFFSET);
  *frac = usec_to_ntp_frac(tv.tv_usec);
}

// Create and bind UDP socket
static int create_ntp_socket(void) {
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  // Set SO_REUSEADDR to allow quick restart
  int opt = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt SO_REUSEADDR");
    close(sockfd);
    return -1;
  }

#ifdef SO_REUSEPORT
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    perror("setsockopt SO_REUSEPORT");
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
    perror("bind");
    close(sockfd);
    return -1;
  }

  return sockfd;
}

// Handle NTP request and send response
static void handle_ntp_request(int sockfd, struct sockaddr_in *client_addr,
  socklen_t client_len, uint8_t stratum) {
  char buf[NTP_PACKET_SIZE];
  struct ntp_packet *packet = (struct ntp_packet *)buf;
  struct timeval tv_recv, tv_xmit;
  
  // Get receive timestamp BEFORE reading (as close as possible to actual receive time)
  gettimeofday(&tv_recv, NULL);
  
  // Read request
  ssize_t bytes_read = recvfrom(sockfd, buf, sizeof(buf), 0,
    (struct sockaddr *)client_addr, &client_len);
  
  if (bytes_read < (ssize_t)sizeof(struct ntp_packet)) {
    return; // Invalid packet size
  }

  // Check if this is a client request (mode 3)
  uint8_t mode = packet->li_vn_mode & 0x07;
  if (mode != 3) {
    return; // Not a client request
  }

  // Get transmit timestamp
  gettimeofday(&tv_xmit, NULL);
  
  // Prepare response
  struct ntp_packet response;
  memset(&response, 0, sizeof(response));
  
  // LI (2 bits): 0 = no warning
  // VN (3 bits): NTP version 4
  // Mode (3 bits): 4 = server
  response.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | 4;
  
  response.stratum = stratum;
  response.poll = 6; // Default poll interval (64 seconds)
  response.precision = -6; // ~15.26 ms precision (signed char)
  
  // Root delay and dispersion - 0 for local clock
  response.root_delay = htonl(0);
  response.root_dispersion = htonl(0);
  
  // Reference ID - LOCL (local clock)
  response.ref_id = htonl(0x4C4F434C); // "LOCL" in hex
  
  // Reference timestamp (T3) - use current time for local clock
  uint32_t ref_sec, ref_frac;
  get_current_ntp_timestamp(&ref_sec, &ref_frac);
  response.ref_timestamp_sec = htonl(ref_sec);
  response.ref_timestamp_frac = htonl(ref_frac);
  
  // Originate timestamp (T1) - copy from request's transmit timestamp
  response.orig_timestamp_sec = packet->tx_timestamp_sec;
  response.orig_timestamp_frac = packet->tx_timestamp_frac;
  
  // Receive timestamp (T2) - when we received the request
  uint32_t rx_sec = (uint32_t)(tv_recv.tv_sec + NTP_EPOCH_OFFSET);
  uint32_t rx_frac = usec_to_ntp_frac(tv_recv.tv_usec);
  response.rx_timestamp_sec = htonl(rx_sec);
  response.rx_timestamp_frac = htonl(rx_frac);
  
  // Transmit timestamp (T4) - when we send the response
  uint32_t tx_sec = (uint32_t)(tv_xmit.tv_sec + NTP_EPOCH_OFFSET);
  uint32_t tx_frac = usec_to_ntp_frac(tv_xmit.tv_usec);
  response.tx_timestamp_sec = htonl(tx_sec);
  response.tx_timestamp_frac = htonl(tx_frac);

  // Send response
  sendto(sockfd, &response, sizeof(response), 0,
    (struct sockaddr *)client_addr, client_len);
}

// Main NTP server loop
void run_ntp_server(uint8_t stratum, int foreground) {
  int sockfd = create_ntp_socket();
  if (sockfd < 0) {
    fprintf(stderr, "Failed to create NTP server socket\n");
    return;
  }

  if (!foreground) {
    // Daemonize
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      close(sockfd);
      return;
    }
    if (pid > 0) {
      // Parent exits
      close(sockfd);
      return;
    }

    // Child continues as daemon
    if (setsid() < 0) {
      perror("setsid");
      close(sockfd);
      exit(1);
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) close(fd);
    }
    chdir("/");
  }

  // Set up signal handlers
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  signal(SIGQUIT, handle_signal);

  fprintf(stderr, "NTP server started on port %d, stratum %d\n", NTP_PORT, stratum);

  while (running) {
    char buf[NTP_PACKET_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    ssize_t bytes_read = recvfrom(sockfd, buf, sizeof(buf), 0,
      (struct sockaddr *)&client_addr, &client_len);
    
    if (bytes_read < 0) {
      if (errno == EINTR) continue;
      perror("recvfrom");
      continue;
    }
    
    if (bytes_read >= (ssize_t)sizeof(struct ntp_packet)) {
      handle_ntp_request(sockfd, &client_addr, client_len, stratum);
    }
  }

  close(sockfd);
  fprintf(stderr, "NTP server stopped\n");
}
