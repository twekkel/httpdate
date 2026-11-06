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

// NTP packet structure (simplified)
struct ntp_packet {
  uint8_t li_vn_mode;      // Leap indicator, version, mode
  uint8_t stratum;         // Stratum level
  uint8_t poll;            // Poll interval
  uint8_t precision;       // Precision
  uint32_t root_delay;     // Root delay
  uint32_t root_dispersion; // Root dispersion
  uint32_t ref_id;         // Reference ID
  uint64_t ref_timestamp;  // Reference timestamp
  uint64_t orig_timestamp; // Originate timestamp
  uint64_t rx_timestamp;   // Receive timestamp
  uint64_t tx_timestamp;   // Transmit timestamp
} __attribute__((packed));

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

// Convert Unix time to NTP timestamp format
static uint64_t unix_to_ntp(time_t unix_time) {
  return ((uint64_t)(unix_time + 2208988800UL)) << 32;
}

// Get current NTP timestamp
static uint64_t get_current_ntp_timestamp(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  
  uint64_t ntp_time = unix_to_ntp(tv.tv_sec);
  // Add fractional seconds (nanoseconds precision)
  uint64_t frac = ((uint64_t)tv.tv_usec << 32) / 1000000UL;
  return ntp_time | (frac & 0xFFFFFFFFULL);
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
  
  // Get current time
  uint64_t now = get_current_ntp_timestamp();
  
  // Copy originate timestamp from request
  response.orig_timestamp = packet->tx_timestamp;
  
  // Set receive timestamp (when we received the request)
  // For simplicity, we use the same as transmit timestamp
  response.rx_timestamp = now;
  
  // Set transmit timestamp (when we send the response)
  response.tx_timestamp = now;
  
  // Reference timestamp - use current time as reference
  response.ref_timestamp = now;
  
  // Reference ID - use LOCL (local clock) reference
  // LOCL is represented as ASCII "LOCL" in network byte order
  response.ref_id = htonl(0x4C4F434C); // "LOCL" in hex

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
      fprintf(stderr, "dummy line to get a response, without I get no eligible servers\n");
      handle_ntp_request(sockfd, &client_addr, client_len, stratum);
    }
  }

  close(sockfd);
  fprintf(stderr, "NTP server stopped\n");
}
