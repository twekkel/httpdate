#define _DEFAULT_SOURCE

#include <curl/curl.h>
#if LIBCURL_VERSION_NUM < 0x073d00
#error "httpdate needs libcurl 7.61.0 or newer"
#endif
#include "ntpd.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
static inline int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req, struct timespec *rem) {
  (void)clock_id; (void)rem;
  if (flags & TIMER_ABSTIME) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    time_t diff_sec = req->tv_sec - now.tv_sec;
    long diff_nsec = req->tv_nsec - now.tv_nsec;
    if (diff_nsec < 0) { diff_sec--; diff_nsec += 1000000000L; }
    if (diff_sec < 0 || (diff_sec == 0 && diff_nsec <= 0)) return 0;
    struct timespec ts = { .tv_sec = diff_sec, .tv_nsec = diff_nsec };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
  } else {
    struct timespec ts = *req;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
  }
  return 0;
}
#endif

#include <sys/timex.h>

#if defined __NetBSD__ || defined __FreeBSD__ || defined __APPLE__
#define adjtimex ntp_adjtime
#endif

#define DEFAULT_MIN_SLEEP       10              // 2^10  ~17 minutes
#define DEFAULT_MAX_SLEEP       14              // 2^14  ~4.5 hours
#define DEFAULT_PRECISION       3               // 3  +/- 125 ms
#define DRIFT_TAU               14400.0         // 4-hour smoothing
#define MAX_OFFSET              1000L
#define MAX_PPM                 2
#define MAX_URLS                8
#define NS_SEC                  1000000000L
#define MAX_DRIFT               32768000L       // 500 PPM
#define VERSION                 "1.0.1"

enum cert_mode {
  CERT_CHECK_ENABLED,
  CERT_CHECK_DISABLED
};

enum run_mode {
  RUN_ONESHOT,
  RUN_BACKGROUND,
  RUN_FOREGROUND
};

enum sanity_mode {
  SANITY_CHECK_ENABLED,
  SANITY_CHECK_DISABLED
};

enum sync_mode {
  SYNC_MODE_NONE,
  SYNC_MODE_SET,
  SYNC_MODE_ADJUST,
  SYNC_MODE_DRIFT
};

static enum cert_mode cert_mode = CERT_CHECK_ENABLED;
static enum run_mode run_mode = RUN_ONESHOT;
static enum sanity_mode sanity_mode = SANITY_CHECK_ENABLED;
static enum sync_mode sync_mode = SYNC_MODE_NONE;

static const char *proxy = NULL;
static int verbose       = 0;
static pid_t ntp_pid     = -1;
static long ip_resolve   = CURL_IPRESOLVE_WHATEVER;
static int ntp_server    = 0;

#define HISTORY_SIZE 8

struct timesample {
  const char *url;
  double offset;
  double rtt;
  double jitter; // Historical jitter of this URL, 0.0 when unknown
};

/* Per-URL history of the most recent offsets, used to estimate the
   quality (jitter) of each server. */
struct url_stats {
  double offsets[HISTORY_SIZE];
  int count;
  int next;
};

static int num(const char *p, int n) {
  int v = 0;
  while (n--) {
    unsigned d = (unsigned char)*p++ - '0';
    if (d > 9) return -1;
    v = v * 10 + (int)d;
  }
  return v;
}

// "DD Mon YYYY HH:MM:SS" (UTC) -> Unix time, or -1 on error
static long long parse_imf_fixdate(const char *s) {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  static const short cum[13] = {0,31,59,90,120,151,181,212,243,273,304,334,365};

  const int d = num(s, 2);
  if (d < 1 || s[2] != ' ')
    return -1;

  int mon;
  for (mon = 0; mon < 12; mon++)
    if (strncmp(s + 3, months + 3 * mon, 3) == 0)
      break;
  if (mon == 12 || s[6] != ' ')
    return -1;

  const int y = num(s + 7, 4);
  if (y < 1970 || s[11] != ' ')
    return -1;

  const int h = num(s + 12, 2);
  if (h < 0 || h > 23 || s[14] != ':')
    return -1;

  const int mi = num(s + 15, 2);
  if (mi < 0 || mi > 59 || s[17] != ':')
    return -1;

  const int sec = num(s + 18, 2);
  if (sec < 0 || sec > 59)
    return -1;

  const int leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  if (d > cum[mon + 1] - cum[mon] + (leap && mon == 1))
    return -1;

  const long long days = 365LL * (y - 1970)
                       + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400 - 477
                       + cum[mon] + (leap && mon > 1) + (d - 1);
  return ((days * 24 + h) * 60 + mi) * 60 + sec;
}

// Insertion sort is more efficient (and smaller) than qsort for small lists
static void insert_sort(double a[], int length) {
  for (int i = 1; i < length; i++) {
    double value = a[i];
    int j;

    for (j = i - 1; j >= 0 && a[j] > value; j--) {
      a[j + 1] = a[j];
    }
    a[j + 1] = value;
  }
}

static double dabs(double x) {
  return x < 0 ? -x : x;
}

// Seconds -> rounded milliseconds
static long long to_ms(double sec) {
  return (long long)(sec * 1000.0 + (sec < 0 ? -0.5 : 0.5));
}

/* Record a new offset sample for a URL and return the jitter of that URL,
   based on the most recent HISTORY_SIZE samples. The jitter is the median
   absolute deviation (MAD) of the stored offsets. Returns 0.0 when fewer
   than two samples are known. */
static double record_sample(struct url_stats *stats, double offset) {
  stats->offsets[stats->next] = offset;
  stats->next = (stats->next + 1) % HISTORY_SIZE;
  if (stats->count < HISTORY_SIZE) stats->count++;

  if (stats->count < 2)
    return 0.0;

  double tmp[HISTORY_SIZE];
  for (int i = 0; i < stats->count; i++)
    tmp[i] = stats->offsets[i];

  insert_sort(tmp, stats->count);
  double median = tmp[stats->count / 2];

  for (int i = 0; i < stats->count; i++)
    tmp[i] = dabs(stats->offsets[i] - median);

  insert_sort(tmp, stats->count);
  return tmp[stats->count / 2];
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

  struct tm *tm = gmtime(&now.tv_sec);
  if (tm) {
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S GMT", tm);
    print_log(0, "Set time: %s", buffer);
  }

  int ret = clock_settime(CLOCK_REALTIME, &now);

  if (ret) {
    print_log(1, "error setting time: %s", strerror(errno));
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

static int set_status(void) {
  struct timex txc = {0};

  txc.modes = MOD_STATUS | MOD_ESTERROR | MOD_MAXERROR;
  txc.status &= ~STA_UNSYNC;
  if (verbose)
    print_log(0, "Set clock synchronized");

  return adjtimex(&txc);
}

static int adjust_freq(double drift, float confidence) {
  struct timex tmx = {0};
  long old_freq;

  adjtimex(&tmx);
  old_freq = tmx.freq;

  // Clamp in the double domain: converting NaN/out-of-range to long is UB
  double freq_target = 65536e6 * drift * confidence;
  if (freq_target != freq_target) return -1;

  // Slew-rate limit: MAX_PPM change per call
  double max_delta = (double)(MAX_PPM * 65536L);
  if (freq_target > max_delta) {
    freq_target = max_delta;
  } else if (freq_target < -max_delta) {
    freq_target = -max_delta;
  }

  long freq_delta = (long)freq_target;

  tmx.freq = old_freq + freq_delta;

  // Enforce absolute safety bounds
  if ((tmx.freq < -MAX_DRIFT) || (tmx.freq > MAX_DRIFT))
    tmx.freq = (tmx.freq < 0 ? -1 : 1) * MAX_DRIFT;
  tmx.modes = MOD_FREQUENCY;

  if (adjtimex(&tmx) < 0) {
    print_log(1, "Frequency change failed: %s", strerror(errno));
    return -1;
  }

  print_log(0, "Set frequency %li", tmx.freq);
  return 0;
}

static long long get_rtt(CURL *curl) {
  curl_off_t t_pre = 0, t_start = 0;
  curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &t_pre);
  curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &t_start);
  return (long long)(t_start - t_pre) * 1000LL;
}

static CURLcode setup_curl_handle(CURL *curl, struct curl_slist **list, const char *url, char *remotetime) {
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

  if (cert_mode == CERT_CHECK_DISABLED) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  if (verbose > 2) {
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }

  return CURLE_OK;
}

/* Measure the offset of one server.
   Returns 0 on success (results in *offset and *rtt), -1 on failure. */
static int bisect(const char *url, int precision, double *offset, double *rtt) {
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
  if (!curl) {
    return -1;
  }

  if (setup_curl_handle(curl, &list, url, remotetime) != CURLE_OK) {
    goto cleanup;
  }

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
  // Two servers that disagree: trust the one with the lowest RTT + jitter
  if (valid_samples == 2 && dabs(samples[0].offset - samples[1].offset) > 0.5) {
    double q0 = samples[0].rtt + samples[0].jitter;
    double q1 = samples[1].rtt + samples[1].jitter;
    *out = (q0 < q1) ? samples[0].offset : samples[1].offset;
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

    // Weight by inverse RTT + historical jitter; the 1 ms floor avoids
    // division by zero. Servers that are consistently noisy get a lower
    // weight than fast, stable ones.
    double weight = 1.0 / (samples[j].rtt + samples[j].jitter + 0.001);
    weighted_sum += samples[j].offset * weight;
    total_weight += weight;
  }

  if (total_weight <= 0)
    return -1;

  *out = weighted_sum / total_weight;
  return 0;
}

static void show_help(void) {
  printf("httpdate version " VERSION "\n"
         "Usage: httpdate [-46acdDFhnsStvx] [-p #] [-P proxy] <URLs> ...\n\n"
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
         "  -S    run a minimal NTP server (daemon mode)\n"
         "  -t    disable time sanity check (limit: %ld s)\n"
         "  -v    show version\n"
         "  -x    adjust time and frequency to correct clock drift (daemon mode)\n",
         DEFAULT_PRECISION, MAX_OFFSET);
}

// Stop the NTP server child, so it does not outlive this process
static void stop_ntp_server(void) {
  if (ntp_pid > 0) {
    kill(ntp_pid, SIGTERM);
    // Wait until the child is gone, so UDP port 123 is free again
    while (waitpid(ntp_pid, NULL, 0) < 0 && errno == EINTR);
    ntp_pid = -1;
  }
}

// On a termination signal, stop the NTP server child and take the default action
static void handle_signal(int sig) {
  stop_ntp_server();
  signal(sig, SIG_DFL);
  raise(sig);
}

// Reap the NTP server child when it exits before this process
static void handle_sigchld(int sig) {
  (void)sig;
  while (waitpid(-1, NULL, WNOHANG) > 0);
  ntp_pid = -1;
}

static int daemonize(void) {
  pid_t pid = fork();

  if (pid < 0)
    return -1;
  if (pid > 0)
    _exit(0);

  if (setsid() < 0 || chdir("/") < 0)
    return -1;

  int fd = open("/dev/null", O_RDWR);
  if (fd < 0)
    return -1;
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd > STDERR_FILENO)
    close(fd);

  return 0;
}

static int update_clock(double avgtime, int precision, int interval) {
  static time_t starttime = 0;

  if (verbose || run_mode == RUN_ONESHOT)
    print_log(0, "Offset: %lld ms", to_ms(avgtime));

  // Consider +/- .5s a valid sync state
  if (sync_mode == SYNC_MODE_DRIFT && dabs(avgtime) < 0.5)
    set_status();

  // Measurement resolution is +/- 1/2^precision seconds
  double threshold = 1.0 / (double)(1 << precision);

  if (dabs(avgtime) <= threshold) {
    if (interval < DEFAULT_MAX_SLEEP) {
      interval++;
    }
    return interval;
  }

  // Avoid bouncing between upper/lower limit when (almost) in sync
  if (dabs(avgtime) < 1.0 && sync_mode != SYNC_MODE_SET) avgtime /= 2;

  // Too large to slew (broken RTC, -t): step once, then back to normal
  if (sync_mode != SYNC_MODE_NONE && dabs(avgtime) > MAX_OFFSET) {
    if (set_clock(avgtime) == 0)
      sanity_mode = SANITY_CHECK_ENABLED;
    starttime = 0;
    return DEFAULT_MIN_SLEEP;
  }

  if (sync_mode == SYNC_MODE_SET) set_clock(avgtime);
  if (sync_mode == SYNC_MODE_ADJUST || sync_mode == SYNC_MODE_DRIFT) {
    if (adjust_clock(avgtime) < 0)
      print_log(1, "adjtime failed: %s", strerror(errno));
  }

  if (run_mode != RUN_ONESHOT && sync_mode == SYNC_MODE_DRIFT) {
    if (starttime) {
      // Calculate systematic clock drift
      double elapsed = (double)(time(NULL) - starttime);
      double drift = avgtime / elapsed;
      print_log(0, "Drift %.2f PPM, %.2f s/day", drift * 1e6, drift * 86400);

      // Adjust the clock frequency, weighted by elapsed time
      if (adjust_freq(drift, (float)(elapsed / (elapsed + DRIFT_TAU))) < 0)
        print_log(1, "Frequency change failed");
    }
    starttime = time(NULL);
  }

  /* Only force DEFAULT_MIN_SLEEP for large corrections,
     otherwise ease back off
  */
  if (dabs(avgtime) > (threshold * 2.0)) {
    interval = DEFAULT_MIN_SLEEP;
  } else if (interval > DEFAULT_MIN_SLEEP) {
    interval--;
  }

  return interval;
}

int main(int argc, char *argv[]) {
  int       interval   = DEFAULT_MIN_SLEEP;
  int       precision  = DEFAULT_PRECISION;

  int       param;
  while ((param = getopt(argc, argv, "46acdhDFnp:P:sStvx")) != -1)
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
        cert_mode = CERT_CHECK_DISABLED;
        break;
      case 'd':
        if (verbose < 3) verbose++;
        break;
      case 'D':
        run_mode = RUN_BACKGROUND;
        set_log_dest(LOG_DEST_SYSLOG);
        if (sync_mode != SYNC_MODE_DRIFT) sync_mode = SYNC_MODE_ADJUST;
        break;
      case 'F':
        run_mode = RUN_FOREGROUND;
        set_log_dest(LOG_DEST_CONSOLE);
        if (sync_mode != SYNC_MODE_DRIFT) sync_mode = SYNC_MODE_ADJUST;
        break;
      case 'h':
        show_help();
        exit(0);
      case 'n':
        if (!proxy) proxy = "";
        break;
      case 'p':
        precision = atoi(optarg);
        if (precision < 1 || precision > 9) {
          fprintf(stderr, "Error: precision must be between 1 and 9\n");
          exit(1);
        }
        break;
      case 'P':
        proxy = optarg;
        break;
      case 's':
        sync_mode = SYNC_MODE_SET;
        break;
      case 'S':
        ntp_server = 1;
        break;
      case 't':
        sanity_mode = SANITY_CHECK_DISABLED;
        break;
      case 'v':
        printf("httpdate version %s\n", VERSION);
        exit(0);
      case 'x':
        sync_mode = SYNC_MODE_DRIFT;
        precision = 9;
        break;
      case '?':
      default:
        show_help();
        exit(1);
    }

  if (argv[optind] == NULL) {
    show_help();
    exit(1);
  }

  if (ntp_server && run_mode == RUN_ONESHOT) {
    fprintf(stderr, "Error: -S is only valid in daemon mode (-D or -F)\n");
    exit(1);
  }

  if (argc - optind > MAX_URLS) {
    print_log(1, "Too many URLs specified");
    exit(1);
  }

  switch (run_mode) {
  case RUN_ONESHOT:
    // Run once without daemonizing or startup logging.
    break;

  case RUN_BACKGROUND:
    openlog("httpdate", LOG_PID | LOG_CONS, LOG_DAEMON);

    if (daemonize() < 0) {
      print_log(1, "Daemon initialization failed: %s", strerror(errno));
      exit(1);
    }

    print_log(0, "httpdate version "VERSION" started");
    break;

  case RUN_FOREGROUND:
    print_log(0, "httpdate version "VERSION" started");
    break;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Start NTP server if -S option was specified
  if (ntp_server) {
    int ready[2];
    if (pipe(ready) < 0) {
      print_log(1, "Failed to create pipe for NTP server: %s", strerror(errno));
      exit(1);
    }
    pid_t pid = fork();
    if (pid < 0) {
      print_log(1, "Failed to fork for NTP server: %s", strerror(errno));
      exit(1);
    }
    if (pid == 0) {
      // Child process: run NTP server in foreground
      close(ready[0]);
      exit(run_ntp_server(ready[1]));
    }
    // Parent: wait for the NTP server to report that it is listening
    close(ready[1]);
    char ready_byte;
    ssize_t nread;
    do {
      nread = read(ready[0], &ready_byte, 1);
    } while (nread < 0 && errno == EINTR);
    close(ready[0]);
    if (nread != 1) {
      print_log(1, "NTP server failed to start");
      curl_global_cleanup();
      exit(1);
    }
    ntp_pid = pid;

    // Stop the NTP server on termination, and reap it when it exits first
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = handle_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    print_log(0, "NTP server started");
  }

  // Per-URL offset history, kept across iterations to track server quality
  struct url_stats stats[MAX_URLS] = {0};

  do {
    struct timesample samples[MAX_URLS] = {0};
    int valid_samples = 0;
    int have_time = 0;
    double avgtime = 0.0;

    for (int i = optind; i < argc; i++) {
      double offset = 0.0;
      double rtt = 0.0;

      if (bisect(argv[i], precision, &offset, &rtt) < 0)
        continue;

      if (sanity_mode == SANITY_CHECK_ENABLED && dabs(offset) > MAX_OFFSET) {
        if (verbose)
          print_log(0, "%-25s | Offset exceeds %ld s, ignored", argv[i], MAX_OFFSET);
        continue;
      }

      // Record the sample and get the URL's historical jitter
      double jitter = record_sample(&stats[i - optind], offset);

      if (verbose) print_log(0, "%-25s | Offset: %lld ms | RTT: %lld ms | Jitter: %lld ms",
                              argv[i], to_ms(offset), to_ms(rtt), to_ms(jitter));
      samples[valid_samples].url = argv[i];
      samples[valid_samples].offset = offset;
      samples[valid_samples].rtt = rtt;
      samples[valid_samples].jitter = jitter;
      valid_samples++;
    }

    if (valid_samples > 0) {
      if (calculate_offset(samples, valid_samples, &avgtime) == 0)
        have_time = 1;
      else
        print_log(1, "No servers passed RTT quality and consensus filters");
    } else {
      print_log(1, "No suitable servers available");
    }

    if (have_time) {
      interval = update_clock(avgtime, precision, interval);
    } else if (run_mode == RUN_ONESHOT) {
      curl_global_cleanup();
      exit(1);
    }

    if (run_mode != RUN_ONESHOT) {
      sleep(1<<interval);
    }

  } while (run_mode != RUN_ONESHOT);

  curl_global_cleanup();
  return 0;
}


