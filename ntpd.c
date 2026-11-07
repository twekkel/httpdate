#define _DEFAULT_SOURCE

#include "ntpd.h"

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
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NTP_EPOCH_OFFSET 2208988800ULL
#define NTP_PACKET_SIZE 48
#define NTP_PORT 123
#define NTP_STRATUM 5
#define NTP_VERSION 4

/* NTP packet, as defined by RFC 5905. All multi-byte fields are on the wire
 * in network byte order. */
struct ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    int8_t precision;
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

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static uint32_t usec_to_ntp_frac(suseconds_t usec)
{
    return (uint32_t)(((uint64_t)usec << 32) / 1000000ULL);
}

static void timeval_to_ntp(const struct timeval *tv,
                           uint32_t *sec, uint32_t *frac)
{
    *sec = (uint32_t)((uint64_t)tv->tv_sec + NTP_EPOCH_OFFSET);
    *frac = usec_to_ntp_frac(tv->tv_usec);
}

static int create_ntp_socket(void)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return -1;
    }

#ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT,
                   &opt, sizeof(opt)) < 0) {
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

/* The main loop receives the datagram exactly once and passes its contents
 * and receive timestamp here. */
static void handle_ntp_request(int sockfd,
                               const struct ntp_packet *request,
                               const struct sockaddr_in *client_addr,
                               socklen_t client_len,
                               const struct timeval *tv_recv,
                               uint8_t stratum)
{
    unsigned int version = (request->li_vn_mode >> 3) & 0x07;
    unsigned int mode = request->li_vn_mode & 0x07;

    if (mode != 3 || (version != 3 && version != 4))
        return;

    struct timeval tv_xmit;
    if (gettimeofday(&tv_xmit, NULL) < 0) {
        perror("gettimeofday");
        return;
    }

    struct ntp_packet response;
    memset(&response, 0, sizeof(response));

    /* LI=0 (no warning), VN=4, Mode=4 (server). */
    response.li_vn_mode = (uint8_t)((NTP_VERSION << 3) | 4);
    response.stratum = NTP_STRATUM;
    response.poll = 6;
    response.precision = -6;

    /* A local-clock reference. These values do not imply synchronization
     * to an external source. */
    response.root_delay = htonl(0);
    response.root_dispersion = htonl(0);
    response.ref_id = htonl(0x4C4F434C); /* "LOCL" */

    uint32_t ref_sec, ref_frac;
    timeval_to_ntp(&tv_xmit, &ref_sec, &ref_frac);
    response.ref_timestamp_sec = htonl(ref_sec);
    response.ref_timestamp_frac = htonl(ref_frac);

    /* Originate timestamp must be copied verbatim from the request. */
    response.orig_timestamp_sec = request->tx_timestamp_sec;
    response.orig_timestamp_frac = request->tx_timestamp_frac;

    uint32_t rx_sec, rx_frac;
    timeval_to_ntp(tv_recv, &rx_sec, &rx_frac);
    response.rx_timestamp_sec = htonl(rx_sec);
    response.rx_timestamp_frac = htonl(rx_frac);

    uint32_t tx_sec, tx_frac;
    timeval_to_ntp(&tv_xmit, &tx_sec, &tx_frac);
    response.tx_timestamp_sec = htonl(tx_sec);
    response.tx_timestamp_frac = htonl(tx_frac);

    ssize_t sent = sendto(sockfd, &response, sizeof(response), 0,
                          (const struct sockaddr *)client_addr, client_len);
    if (sent < 0) {
        perror("sendto");
    } else if ((size_t)sent != sizeof(response)) {
        fprintf(stderr, "sendto: incomplete NTP response\n");
    }
}

static int run_ntp_server(int foreground) {
    running = 1;

    int sockfd = create_ntp_socket();
    if (sockfd < 0) {
        fprintf(stderr, "Failed to create NTP server socket\n");
        return;
    }

    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(sockfd);
            return;
        }
        if (pid > 0) {
            close(sockfd);
            return;
        }

        if (setsid() < 0) {
            perror("setsid");
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
        if (setsid() < 0 || chdir("/") < 0)
          return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    fprintf(stderr, "NTP server started on UDP port %d, stratum %u\n",
            NTP_PORT, NTP_STRATUM);

    while (running) {
        /* Leave room for NTP extension fields; only the first 48 bytes are
         * the fixed header used by this minimal server. */
        uint8_t buf[512];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        ssize_t bytes_read = recvfrom(sockfd, buf, sizeof(buf), 0,
                                      (struct sockaddr *)&client_addr,
                                      &client_len);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            if (!running)
                break;
            perror("recvfrom");
            continue;
        }

        struct timeval tv_recv;
        if (gettimeofday(&tv_recv, NULL) < 0) {
            perror("gettimeofday");
            continue;
        }

        if ((size_t)bytes_read < sizeof(struct ntp_packet))
            continue;

        /* Copy the fixed header to an aligned local object. */
        struct ntp_packet request;
        memcpy(&request, buf, sizeof(request));

        handle_ntp_request(sockfd, &request, &client_addr, client_len,
                           &tv_recv, NTP_STRATUM);
    }

    close(sockfd);
    fprintf(stderr, "NTP server stopped\n");
}
