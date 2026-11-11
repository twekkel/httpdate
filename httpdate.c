#define _DEFAULT_SOURCE

#include <curl/curl.h>
#if LIBCURL_VERSION_NUM < 0x073d00
#error "httpdate needs libcurl 7.61.0 or newer"
#endif
#include "log.h"
#include "utils.h"
#ifdef WITH_NTP
#include "ntpd.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <syslog.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>


#if defined __NetBSD__ || defined __FreeBSD__
#define adjtimex ntp_adjtime
#endif

#define DEFAULT_MAX_SLEEP       14              // 2^14  ~4.5 hours
#define DEFAULT_MIN_SLEEP       10              // 2^10  ~17 minutes
#define DEFAULT_PRECISION       3               // 3  +/- 125 ms
#define DRIFT_TAU               14400.0         // 4-hour smoothing
#define DRIFT_TO_FREQ           (1e6 * 65536.0)
#define MAX_DRIFT               32768000L       // 500 PPM
#define STEP_THRESHOLD          1.0             // Offsets > threshold: stepped
#define MAX_OFFSET              1000L
#define MAX_PPM                 2
#define MAX_URLS                8
#define NS_SEC                  1000000000L
#define VERSION                 "1.1.0"

enum sync_mode {
  SYNC_MODE_NONE,
  SYNC_MODE_SET,
  SYNC_MODE_ADJUST,
  SYNC_MODE_DRIFT
};
static enum sync_mode sync_mode = SYNC_MODE_NONE;

static bool background   = false;
static bool cert_mode    = true;
static bool daemon_mode  = false;   // -D or -F
static bool ntp_server   = false;
static bool sanity_mode  = true;
static const char *proxy = NULL;
static int verbose       = 0;
static long ip_resolve   = CURL_IPRESOLVE_WHATEVER;
#ifdef WITH_NTP
static volatile sig_atomic_t ntp_pid  = -1;  // written by signal handlers
static volatile sig_atomic_t ntp_died = 0;   // set when the NTP child exits
#endif

struct timesample {
  const char *url;
  double offset;
  double rtt;
  long long resolution;
};

static inline bool is_slew_mode(void) {
  return sync_mode == SYNC_MODE_ADJUST || sync_mode == SYNC_MODE_DRIFT;
}

static inline bool is_drift_mode(void) {
  return sync_mode == SYNC_MODE_DRIFT;
}

static size_t get_remote_time(void *contents, size_t size,
  size_t nmemb, void *userdata) {
  const char *buffer = contents;
  char *remotetime   = userdata;
  size_t len         = size * nmemb;

  if (len >= 6 + 5 + 20 && strncasecmp(buffer, "date: ", 6) == 0) {
    memcpy(remotetime, buffer + 11, 20);
    remotetime[20] = '\0';
  }

  return len;
}

static long long offset_sec(const char *remotetime, time_t local_sec) {
  const long long remote = parse_imf_fixdate(remotetime);
  return remote < 0 ? LLONG_MAX : (long long)(local_sec - remote);
}

static int set_clock(double timedelta) {
  struct timespec now;
  char   buffer[32] = {0};

  clock_gettime(CLOCK_REALTIME, &now);
  timedelta += ((double)now.tv_sec + (double)now.tv_nsec * 1e-9);

  now.tv_sec  = (time_t)timedelta;
  now.tv_nsec = (long)((timedelta - (double)now.tv_sec) * 1e9);

  int ret = clock_settime(CLOCK_REALTIME, &now);

  if (ret) {
    print_log(1, "error setting time: %s", strerror(errno));
    return ret;
  }

  // Log the new time only after the clock change succeeded
  struct tm *tm = gmtime(&now.tv_sec);
  if (tm) {
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S GMT", tm);
    print_log(0, "Set time: %s", buffer);
  }

  return ret;
}

static int adjust_clock(double timedelta) {
  struct timeval    timeofday;

  print_log(0, "Adjusting %lld ms", to_ms(timedelta));

  timeofday.tv_sec  = (time_t)timedelta;
  timeofday.tv_usec = (long)((timedelta - (double)timeofday.tv_sec) * 1e6L);
  if (timeofday.tv_usec < 0) {
    timeofday.tv_usec += 1000000L;
    timeofday.tv_sec--;
  }

  return adjtime(&timeofday, NULL);
}

// Mark the clock synchronized (status 0 clears STA_UNSYNC) and report the
// error estimate: all arguments in seconds
static int set_status(double offset, double resolution, double rtt) {
  struct timex txc = {0};

  txc.modes = MOD_STATUS | MOD_ESTERROR | MOD_MAXERROR;
  txc.esterror = (long)(resolution / 2 * 1e6);
  txc.maxerror = (long)((dabs(offset) + resolution / 2 + rtt / 2) * 1e6);
  if (verbose)
    print_log(0, "Set clock synchronized");

  return adjtimex(&txc);
}

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static int adjust_freq(double drift, double confidence) {
  struct timex tmx = {0};

  if (adjtimex(&tmx) < 0) {
    print_log(1, "adjtimex read failed: %s", strerror(errno));
    return -1;
  }

  double target = DRIFT_TO_FREQ * drift * confidence;
  if (target != target) {  // NaN
    print_log(1, "invalid drift/confidence, skipping");
    return -1;
  }

  double max_delta = (double)MAX_PPM * 65536.0;
  long delta = (long)clampd(target, -max_delta, max_delta);
  tmx.freq = (long)clampd((double)(tmx.freq + delta), -MAX_DRIFT, MAX_DRIFT);
  tmx.modes = MOD_FREQUENCY;

  if (adjtimex(&tmx) < 0) {
    print_log(1, "Frequency change failed: %s", strerror(errno));
    return -1;
  }

  print_log(0, "Set frequency %li", tmx.freq);
  return 0;
}

// The round trip time of the request: valid per transfer, also when the
// keep-alive connection is reused (connect/lookup timers would stay 0)
static long long get_rtt(CURL *curl) {
  curl_off_t t_start = 0, t_pre = 0;
  curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &t_start);
  curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &t_pre);
  return (long long)(t_start - t_pre) * 1000LL;
}

static void setup_curl_handle(CURL *curl, struct curl_slist **list, const char *url, char *remotetime) {
  *list = curl_slist_append(*list, "Cache-Control: no-cache");

  if (proxy)
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *list);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, get_remote_time);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, remotetime);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, ip_resolve);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "httpdate/"VERSION);

  if (!cert_mode) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  if (verbose > 2) {
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }
}

// Measure the offset of one server.
// Returns 0 on success, -1 on failure.
static int bisect(const char *url, int precision, double *offset, double *rtt,
  int *polls_done) {
  CURL *curl = NULL;
  struct curl_slist *list = NULL;
  CURLcode res;
  struct timespec wake;
  char remotetime[21] = {0};
  int ret = -1;
  double last_rtt = 0.0;
  double total_rtt = 0.0;
  double min_rtt = 0.0, max_rtt = 0.0;
  long long sec_offset = 0, first_offset = 0, prev_offset = 0;
  long long nap = NS_SEC;
  long long rtt_ns = 0;
  long long when = nap >> precision;
  long long target;
  int polls = 0;

  curl = curl_easy_init();
  if (!curl) return -1;

  setup_curl_handle(curl, &list, url, remotetime);

  // Eliminate connection setup time (DNS, TCP, TLS) before the poll loop.
  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    print_log(1, "failed: %s %s", url, curl_easy_strerror(res));
    goto cleanup;
  }
  rtt_ns = get_rtt(curl);
  last_rtt = (double)rtt_ns * 1e-9;
  min_rtt = max_rtt = last_rtt;

  do {
    if (verbose > 1)
      print_log(0, "bisect: %i, when: %09lli", polls, when);

    // Wait till we reach the desired time, "when" - latency
    target = (when - rtt_ns / 2) % NS_SEC;
    if (target < 0) target += NS_SEC;

    // Absolute wake time: next moment the wall clock reads "target" ns
    clock_gettime(CLOCK_REALTIME, &wake);
    if (wake.tv_nsec > target) wake.tv_sec++;
    wake.tv_nsec = target;

    // Local second in which the request reaches the server
    time_t local_sec = wake.tv_sec + ((long long)wake.tv_nsec + rtt_ns / 2) / NS_SEC;

    while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &wake, NULL) == EINTR)
      ;

    remotetime[0] = '\0';
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      print_log(1, "failed: %s %s", url, curl_easy_strerror(res));
      goto cleanup;
    }

    // rtt_ns contains round trip time in nanoseconds
    rtt_ns = get_rtt(curl);
    last_rtt = (double)rtt_ns * 1e-9;
    if (last_rtt < min_rtt) min_rtt = last_rtt;
    if (last_rtt > max_rtt) max_rtt = last_rtt;

    if (remotetime[0] == '\0') {
      print_log(1, "%s no timestamp", url);
      goto cleanup;
    }

    sec_offset = offset_sec(remotetime, local_sec);
    if (sec_offset == LLONG_MAX) {
      print_log(1, "%s unknown time format", url);
      goto cleanup;
    }

    polls++;
    total_rtt += last_rtt;

    nap >>= 1;
    if (polls > 1) {
      if (sec_offset != prev_offset) nap = -nap;
    } else {
      first_offset = sec_offset;
    }
    prev_offset = sec_offset;

    if (verbose > 1) {
      print_log(0, "%-25s %s (%lld ms) => %lld", url, remotetime,
        rtt_ns / 1000000L, sec_offset);
    }

    when += nap;

    // Stop if the RTT jitter exceeds the current bisection step size
    double jitter = max_rtt - min_rtt;
    double step_size = (double)llabs(nap) / NS_SEC;
    if (jitter > step_size) {
      if (verbose > 1)
        print_log(0, "%s RTT jitter %lld ms exceeds step size %lld ms",
          url, to_ms(jitter), to_ms(step_size));
      break;
    }
  } while (polls < precision);

  if (verbose > 1) print_log(0, "when: %lld, nap: %lld", when, nap);

  *polls_done = polls;

  *offset = -(double)first_offset + 1.0 - (double)when / (double)NS_SEC;

  // Average RTT is only used to weight the servers in main()
  *rtt = total_rtt / polls;

  ret = 0;

cleanup:
  if (list) curl_slist_free_all(list);
  if (curl) curl_easy_cleanup(curl);

  return ret;
}

static int calculate_offset(struct timesample samples[], int valid_samples,
  double *out) {
  // Two servers that disagree: trust the one with the lowest RTT
  if (valid_samples == 2 && dabs(samples[0].offset - samples[1].offset) > 0.5) {
    *out = (samples[0].rtt < samples[1].rtt) ? samples[0].offset : samples[1].offset;
    return 0;
  }

  double tmp[MAX_URLS];
  for (int j = 0; j < valid_samples; j++)
    tmp[j] = samples[j].offset;

  insert_sort(tmp, valid_samples);
  double median_offset = tmp[valid_samples / 2];

  // Use Median Absolute Deviation (MAD) for outlier detection
  for (int j = 0; j < valid_samples; j++)
    tmp[j] = dabs(samples[j].offset - median_offset);

  insert_sort(tmp, valid_samples);
  double mad = tmp[valid_samples / 2];

  // Drop samples more than 3 MADs away from the median
  double threshold = 3.0 * mad;

  double weighted_sum = 0.0;
  double total_weight = 0.0;

  for (int j = 0; j < valid_samples; j++) {
    if (dabs(samples[j].offset - median_offset) > threshold) {
      if (verbose)
        print_log(0, "%-25s | Dropping outlier: %lld ms from median",
          samples[j].url, to_ms(samples[j].offset - median_offset));
      continue;
    }

    // Weight by inverse RTT; the 1 ms floor avoids division by zero
    double weight = 1.0 / (samples[j].rtt + 0.001);
    weighted_sum += samples[j].offset * weight;
    total_weight += weight;
  }

  if (total_weight <= 0)
    return -1;

  *out = weighted_sum / total_weight;
  return 0;
}

static void show_help(void) {
  printf("httpdate version "VERSION"\n"
#ifdef WITH_NTP
         "Usage: httpdate [-46acdDFhnsStvx] [-p #] [-P proxy] <URLs> ...\n\n"
#else
         "Usage: httpdate [-46acdDFhnstvx] [-p #] [-P proxy] <URLs> ...\n\n"
#endif
         "  -4    force IPv4 resolution\n"
         "  -6    force IPv6 resolution\n"
         "  -a    adjust time gradually\n"
         "  -c    disable SSL certificate check\n"
         "  -d    debug/verbose output\n"
         "  -D    run as daemon\n"
         "  -F    run daemon in foreground\n"
         "  -h    help\n"
         "  -n    ignore proxy environment variables\n"
         "  -p    precision (1..9, default: %d)\n"
         "  -P    use proxy server (e.g., http://proxy:port or host:port)\n"
         "  -s    set time\n"
#ifdef WITH_NTP
         "  -S    run a minimal NTP server (daemon mode)\n"
#endif
         "  -t    disable time sanity check (limit: %ld s)\n"
         "  -v    show version\n"
         "  -x    adjust time and frequency to correct clock drift (daemon mode)\n",
         DEFAULT_PRECISION, MAX_OFFSET);
}

#ifdef WITH_NTP
// Stop the NTP server child, so it does not outlive this process
static void stop_ntp_server(void) {
  if (ntp_pid > 0) {
    kill((pid_t)ntp_pid, SIGTERM);
    // Wait until the child is gone, so UDP port 123 is free again
    while (waitpid((pid_t)ntp_pid, NULL, 0) < 0 && errno == EINTR);
    ntp_pid = -1;
  }
}

// On SIGTERM, stop NTP server
static void handle_signal(int sig) {
  stop_ntp_server();
  signal(sig, SIG_DFL);
  raise(sig);
}

// Flag the NTP child's exit and main loop logs it
static void handle_sigchld(int sig) {
  (void)sig;
  while (waitpid(-1, NULL, WNOHANG) > 0);
  ntp_pid = -1;
  ntp_died = 1;
}
#endif

// Read one byte from fd (retry on EINTR), then close it. Returns 1 on success.
static int read_byte(int fd, char *c) {
  ssize_t n;
  do {
    n = read(fd, c, 1);
  } while (n < 0 && errno == EINTR);
  close(fd);
  return n == 1;
}

#ifdef WITH_NTP
// Fork the NTP server, wait until it listens, and install the signal handlers.
// Returns 0 on success, -1 on failure.
static int start_ntp_server(void) {
  int ready[2];
  if (pipe(ready) < 0) {
    print_log(1, "Failed to create pipe for NTP server: %s", strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    print_log(1, "Failed to fork for NTP server: %s", strerror(errno));
    close(ready[0]);
    close(ready[1]);
    return -1;
  }
  if (pid == 0) {
    close(ready[0]);
    _exit(run_ntp_server(ready[1]));
  }

  close(ready[1]);
  char c;
  if (!read_byte(ready[0], &c)) {
    print_log(1, "NTP server failed to start");
    return -1;
  }
  ntp_pid = pid;

  // Stop the NTP server on termination, and reap it when it exits first
  struct sigaction sa = {0};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sa.sa_handler = handle_sigchld;
  sigaction(SIGCHLD, &sa, NULL);

  return 0;
}
#endif

// Child reports startup status through this fd; the parent exits 0 or 1.
static int confirm_fd = -1;

// Detach from the terminal. The parent waits for startup, including the NTP
// bind, so "httpdate -D" returns the actual startup result.
static void daemonize(void) {
  int startup[2];
  if (pipe(startup) < 0) {
    print_log(1, "Failed to create pipe for daemon startup: %s", strerror(errno));
    exit(1);
  }

  pid_t pid = fork();
  if (pid < 0) {
    print_log(1, "Failed to fork for daemon: %s", strerror(errno));
    exit(1);
  }
  if (pid > 0) {
    close(startup[1]);
    char c = '0';
    if (!read_byte(startup[0], &c) || c != '1') {
      print_log(1, "Daemon startup failed");
      exit(1);
    }
    _exit(0);
  }

  close(startup[0]);
  if (setsid() < 0 || chdir("/") < 0) {
    print_log(1, "Daemon initialization failed: %s", strerror(errno));
    close(startup[1]);
    exit(1);
  }

  int fd = open("/dev/null", O_RDWR);
  if (fd < 0) {
    print_log(1, "Failed to open /dev/null: %s", strerror(errno));
    close(startup[1]);
    exit(1);
  }
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd > STDERR_FILENO)
    close(fd);

  confirm_fd = startup[1];
}

// Returns the next sleep interval (a power of two exponent), or -1 on failure
static int update_clock(double avgtime, long long resolution, double rtt, int interval) {
  static time_t starttime = 0;

  if (verbose || !daemon_mode)
    print_log(0, "Offset: %lld ms", to_ms(avgtime));

  double threshold = (double)resolution / NS_SEC;

  // Consider +/- .5s a valid sync state
  if (is_drift_mode() && dabs(avgtime) < 0.5) {
    if (set_status(avgtime, threshold, rtt) < 0)
      print_log(1, "adjtimex status failed: %s", strerror(errno));
  }

  // Increase interval when avgtime is within measurement uncertainty
  if (dabs(avgtime) <= threshold)
    return interval < DEFAULT_MAX_SLEEP ? interval + 1 : interval;

  // Avoid bouncing between upper/lower limit when (almost) in sync
  if (dabs(avgtime) < 1.0 && sync_mode != SYNC_MODE_SET) avgtime /= 2;

  // Too large to slew (adjtime is ~0.5 ms/s): step instead of slewing
  if (is_slew_mode() && dabs(avgtime) > STEP_THRESHOLD) {
    int ret = set_clock(avgtime);
    if (ret == 0)
      sanity_mode = true;
    starttime = 0;
    return ret < 0 ? -1 : DEFAULT_MIN_SLEEP;
  }

  if (sync_mode == SYNC_MODE_SET && set_clock(avgtime) < 0)
    return -1;
  if (is_slew_mode()) {
    if (adjust_clock(avgtime) < 0) {
      print_log(1, "adjtime failed: %s", strerror(errno));
      return -1;
    }
  }

  if (daemon_mode && is_drift_mode()) {
    if (starttime) {
      // Calculate systematic clock drift
      double elapsed = (double)(time(NULL) - starttime);
      double drift = avgtime / elapsed;
      print_log(0, "Drift %.2f PPM, %.2f s/day", drift * 1e6, drift * 86400);

      // Adjust the clock frequency, weighted by elapsed time
      if (adjust_freq(drift, elapsed / (elapsed + DRIFT_TAU)) < 0)
        print_log(1, "Frequency change failed");
    }
    starttime = time(NULL);
  }

  // DEFAULT_MIN_SLEEP for large corrections, otherwise ease back off
  if (dabs(avgtime) > (threshold * 2.0))
    return DEFAULT_MIN_SLEEP;
  return interval > DEFAULT_MIN_SLEEP ? interval - 1 : interval;
}

// Parse the command line into the globals. Returns the precision.
static int parse_args(int argc, char *argv[]) {
  int         precision     = DEFAULT_PRECISION;
  int         precision_set = 0;
  const char *proxy_arg     = NULL;
  bool        no_proxy      = false;
  int         param;

  while ((param = getopt(argc, argv, "46acdDFhnp:P:sStvx")) != -1)
    switch(param) {
      case '4':
        ip_resolve = CURL_IPRESOLVE_V4;
        break;
      case '6':
        ip_resolve = CURL_IPRESOLVE_V6;
        break;
      case 'a':
        sync_mode = SYNC_MODE_ADJUST;
        break;
      case 'c':
        cert_mode = false;
        break;
      case 'd':
        if (verbose < 3) verbose++;
        break;
      case 'D':
        daemon_mode = true;
        background = true;
        set_log_dest(LOG_DEST_SYSLOG);
        if (!is_drift_mode()) sync_mode = SYNC_MODE_ADJUST;
        break;
      case 'F':
        daemon_mode = true;
        set_log_dest(LOG_DEST_CONSOLE);
        if (!is_drift_mode()) sync_mode = SYNC_MODE_ADJUST;
        break;
      case 'h':
        show_help();
        exit(0);
      case 'n':
        no_proxy = true;
        break;
      case 'p': {
        char *endptr;
        precision = (int)strtol(optarg, &endptr, 10);
        if (endptr == optarg || *endptr != '\0' ||
            precision < 1 || precision > 9) {
          fprintf(stderr, "Error: precision must be between 1 and 9\n");
          exit(1);
        }
        precision_set = 1;
        break;
      }
      case 'P':
        proxy_arg = optarg;
        break;
      case 's':
        sync_mode = SYNC_MODE_SET;
        break;
      case 'S':
        ntp_server = true;
        sync_mode  = SYNC_MODE_DRIFT;
        break;
      case 't':
        sanity_mode = false;
        break;
      case 'v':
        printf("httpdate version %s\n", VERSION);
        exit(0);
      case 'x':
        sync_mode = SYNC_MODE_DRIFT;
        break;
      case '?':
      default:
        show_help();
        exit(1);
    }

  // -x uses maximum precision unless -p is explicitly specified.
  if (is_drift_mode() && !precision_set)
    precision = 9;
  // -P overrides -n; otherwise use proxy environment variables.
  proxy = proxy_arg ? proxy_arg : (no_proxy ? "" : NULL);

  if (argv[optind] == NULL) {
    show_help();
    exit(1);
  }

#ifndef WITH_NTP
  if (ntp_server) {
    fprintf(stderr, "Error: NTP server support not compiled in (build with: make WITH_NTP=1)\n");
    exit(1);
  }
#endif

  if (ntp_server && !daemon_mode) {
    fprintf(stderr, "Error: -S is only valid in daemon mode (-D or -F)\n");
    exit(1);
  }

  if (argc - optind > MAX_URLS) {
    print_log(1, "Too many URLs specified");
    exit(1);
  }

  return precision;
}

// Daemonize or log the start, depending on the run mode
static void start_run_mode(void) {
  if (background) {
    openlog("httpdate", LOG_PID, LOG_DAEMON);
    daemonize();
  }
  if (daemon_mode)
    print_log(0, "httpdate version "VERSION" started");
}

// The -D parent is still waiting: confirm that startup succeeded
static void confirm_startup(void) {
  if (confirm_fd < 0) return;
  if (write(confirm_fd, "1", 1) < 0)
    print_log(1, "write confirm byte: %s", strerror(errno));
  close(confirm_fd);
  confirm_fd = -1;
}

// Measure all servers once and combine them into one offset and resolution.
// Returns 0 on success, -1 when no usable time was obtained.
static int poll_servers(int argc, char *argv[], int precision,
  double *avgtime, long long *resolution, double *min_rtt) {
  struct timesample samples[MAX_URLS] = {0};
  int valid_samples = 0;

  for (int i = optind; i < argc; i++) {
    double offset = 0.0;
    double rtt = 0.0;
    int polls_done = 0;

    if (bisect(argv[i], precision, &offset, &rtt, &polls_done) < 0)
      continue;

    if (sanity_mode && dabs(offset) > MAX_OFFSET) {
      if (verbose)
        print_log(0, "%-25s | Offset exceeds %ld s, ignored", argv[i], MAX_OFFSET);
      continue;
    }

    // The offset uncertainty is the last bisection step
    long long res = NS_SEC >> polls_done;

    if (verbose) print_log(0, "%-25s | Offset: %lld ms | RTT: %lld ms | Resolution: %lld ms",
                            argv[i], to_ms(offset), to_ms(rtt), to_ms((double)res / NS_SEC));
    samples[valid_samples].url = argv[i];
    samples[valid_samples].offset = offset;
    samples[valid_samples].rtt = rtt;
    samples[valid_samples].resolution = res;
    valid_samples++;
  }

  if (valid_samples == 0) {
    print_log(1, "No suitable servers available");
    return -1;
  }

  // Use median resolution so one noisy server doesn't skew the measurement
  double tmp[MAX_URLS];
  for (int j = 0; j < valid_samples; j++)
    tmp[j] = (double)samples[j].resolution;
  insert_sort(tmp, valid_samples);
  *resolution = (long long)tmp[valid_samples / 2];

  // The lowest RTT is the most accurate server
  *min_rtt = samples[0].rtt;
  for (int j = 1; j < valid_samples; j++)
    if (samples[j].rtt < *min_rtt) *min_rtt = samples[j].rtt;

  if (calculate_offset(samples, valid_samples, avgtime) < 0) {
    print_log(1, "No servers passed RTT quality and consensus filters");
    return -1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int interval  = DEFAULT_MIN_SLEEP;
  int precision = parse_args(argc, argv);

  start_run_mode();
  curl_global_init(CURL_GLOBAL_DEFAULT);

#ifdef WITH_NTP
  if (ntp_server && start_ntp_server() < 0) {
    curl_global_cleanup();
    exit(1);
  }
#endif

  confirm_startup();

  do {
#ifdef WITH_NTP
    // The NTP child was reaped by the SIGCHLD handler; log it here,
    // where async-unsafe functions are allowed again
    if (ntp_died) {
      ntp_died = 0;
      print_log(1, "NTP server exited unexpectedly");
    }
#endif

    double avgtime = 0.0, rtt = 0.0;
    long long resolution = 0;

    int next = poll_servers(argc, argv, precision, &avgtime, &resolution, &rtt) < 0
             ? -1 : update_clock(avgtime, resolution, rtt, interval);

    if (next < 0 && !daemon_mode) {
      curl_global_cleanup();
      exit(1);
    }
    if (next >= 0)
      interval = next;

    if (daemon_mode)
      sleep(1 << interval);
  } while (daemon_mode);

  curl_global_cleanup();
  return 0;
}
