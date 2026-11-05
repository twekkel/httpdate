#define _GNU_SOURCE

#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if defined __NetBSD__ || defined __FreeBSD__ || defined __APPLE__
#define adjtimex ntp_adjtime
#endif

#define DEFAULT_MIN_SLEEP       10              /* 2^10  ~17 minutes */
#define DEFAULT_MAX_SLEEP       14              /* 2^14  ~4.5 hours  */
#define DEFAULT_PRECISION       3               /* 3  +/- 125 ms     */
#define DRIFT_TAU               14400.0         /* 4-hour smoothing  */
#define PRINTBUFFERSIZE         1024
#define MAX_OFFSET              31536000L       /* 1 year in seconds */
#define MAX_PPM                 2
#define MAX_URLS                16
#define NS_SEC                  1000000000L
#define MAX_DRIFT               32768000L       /* 500 PPM */
#define VERSION                 "1.0.1"

enum cert_mode {
  CERT_CHECK_ENABLED,
  CERT_CHECK_DISABLED
};

enum log_dest {
  LOG_DEST_CONSOLE,
  LOG_DEST_SYSLOG
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
static enum log_dest log_dest = LOG_DEST_CONSOLE;
static enum run_mode run_mode = RUN_ONESHOT;
static enum sanity_mode sanity_mode = SANITY_CHECK_ENABLED;
static enum sync_mode sync_mode = SYNC_MODE_NONE;

static const char *proxy = NULL;
static int verbose       = 0;
static long ip_resolve   = CURL_IPRESOLVE_WHATEVER;

struct timesample {
  double offset;
  double rtt;
};

/* Insertion sort is more efficient (and smaller) than qsort for small lists */
static void insertsort(double a[], int length) {
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

/* Seconds -> rounded milliseconds */
static int to_ms(double sec) {
  return (int)(sec * 1000.0 + (sec < 0 ? -0.5 : 0.5));
}

static void printlog(int is_error, char *format, ...) {
  va_list args;
  char buf[PRINTBUFFERSIZE];

  va_start(args, format);
  (void) vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  switch(log_dest) {
    case LOG_DEST_CONSOLE:
      fprintf(is_error ? stderr : stdout, "%s\n", buf);
      fflush(is_error ? stderr : stdout);
      break;
    case LOG_DEST_SYSLOG:
      syslog(is_error ? LOG_WARNING : LOG_INFO, "%s", buf);
      break;
    default:
      fprintf(stderr, "%s\n", "Invalid logmode, aborting");
      abort();
    }
}

static size_t getremotetime(void *contents, size_t size,
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

static long offset_sec(char remotetime[21], time_t local_sec) {
  struct tm tm = {0};

  if (strptime(remotetime, "%d %b %Y %H:%M:%S", &tm) == NULL) {
    return LONG_MAX;
  }

  time_t remote_epoch = timegm(&tm);
  if (remote_epoch == (time_t)-1) {
    return LONG_MAX;
  }

  return local_sec - (long)remote_epoch;
}

static int setclock(double timedelta) {
  struct timespec now;
  char   buffer[32] = {0};

  clock_gettime(CLOCK_REALTIME, &now);
  timedelta += ((double)now.tv_sec + (double)now.tv_nsec * 1e-9);

  now.tv_sec  = (long)timedelta;
  now.tv_nsec = (long)((timedelta - (double)now.tv_sec) * 1e9);

  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S GMT", gmtime(&now.tv_sec));
  printlog(0, "Set time: %s", buffer);

  int ret = clock_settime(CLOCK_REALTIME, &now);

  if (ret) {
    printlog(1, "error setting time");
  }

  return ret;
}

static int adjustclock(double timedelta) {
  struct timeval    timeofday;
  int               ret;

  printlog(0, "Adjusting %d ms", to_ms(timedelta));

  timeofday.tv_sec  = (long)timedelta;
  timeofday.tv_usec = (long)((timedelta - (double)timeofday.tv_sec) * 1e6L);

  ret = adjtime(&timeofday, NULL);
  return(ret);
}

static int setstatus(void) {
  struct timex txc = {0};

  txc.modes = MOD_STATUS | MOD_ESTERROR | MOD_MAXERROR;
  txc.status &= ~STA_UNSYNC;
  if (verbose)
    printlog(0, "Set clock synchronized");

  return(adjtimex(&txc));
}

static int adjust_freq(double drift, float confidence) {
  struct timex tmx = {0};
  long old_freq;

  /* Read current clock frequency */
  tmx.modes = 0;
  adjtimex(&tmx);
  old_freq = tmx.freq;

  /* Weighted target delta based on measured drift and confidence */
  long freq_delta = (long)(65536e6 * drift * confidence);

  /* Slew-rate limit: MAX_PPM change per call */
  long max_delta = MAX_PPM * 65536L;
  if (freq_delta > max_delta) {
    freq_delta = max_delta;
  } else if (freq_delta < -max_delta) {
    freq_delta = -max_delta;
  }

  tmx.freq = old_freq + freq_delta;

  /* Enforce absolute safety bounds */
  if ((tmx.freq < -MAX_DRIFT) || (tmx.freq > MAX_DRIFT))
    tmx.freq = (tmx.freq < 0 ? -1 : 1) * MAX_DRIFT;
  tmx.modes = MOD_FREQUENCY;

  if (adjtimex(&tmx) < 0) {
    printlog(1, "Frequency change failed: %s", strerror(errno));
    return -1;
  }

  printlog(0, "Set frequency %li", tmx.freq);
  return 0;
}

static long getrtt(CURL *curl, double *rtt_s) {
  curl_off_t t_pre = 0, t_start = 0;
  curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &t_pre);
  curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &t_start);
  *rtt_s = (double)(t_start - t_pre) / 1e6;
  return (long)(t_start - t_pre) * 1000L;
}

static CURLcode setup_curl_handle(CURL *curl, struct curl_slist **list, const char *url, char *remotetime) {
  // "Force" clean timestamp
  *list = curl_slist_append(*list, "Cache-Control: no-cache");

  if (proxy)
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *list);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, getremotetime);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)remotetime);
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
  char remotetime[25] = {0};
  int ret = -1;
  double last_rtt = 0.0;
  double total_rtt = 0.0;
  double min_rtt = 0.0, max_rtt = 0.0;   /* set after the first request */
  long sec_offset = 0, first_offset = 0, prev_offset = 0;
  long nap = NS_SEC;
  long rtt_ns = 0;
  long when = nap >> precision;
  long target;
  int polls = 0;
  int steps = precision;

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
    printlog(1, "failed: %s %s", url, curl_easy_strerror(res));
    goto cleanup;
  }
  rtt_ns = getrtt(curl, &last_rtt);
  min_rtt = max_rtt = last_rtt;

  do {
    if (verbose > 1)
      printlog(0, "bisect: %i, when: %09li", polls, when);

    /* Wait till we reach the desired time, "when" - latency */
    target = (when - rtt_ns / 2) % NS_SEC;
    if (target < 0) target += NS_SEC;

    /* Absolute wake time: next moment the wall clock reads "target" ns */
    clock_gettime(CLOCK_REALTIME, &wake);
    if (wake.tv_nsec > target) wake.tv_sec++;
    wake.tv_nsec = target;

    /* Local second in which the request reaches the server */
    time_t local_sec = wake.tv_sec + ((long long)wake.tv_nsec + rtt_ns / 2) / NS_SEC;

    while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &wake, NULL) == EINTR)
      ;

    /* Send HEAD request */
    remotetime[0] = '\0';
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      printlog(1, "failed: %s %s", url, curl_easy_strerror(res));
      goto cleanup;
    }

    /* rtt_ns contains round trip time in nanoseconds */
    rtt_ns = getrtt(curl, &last_rtt);
    if (last_rtt < min_rtt) min_rtt = last_rtt;
    if (last_rtt > max_rtt) max_rtt = last_rtt;

    if (remotetime[0] == '\0') {
      printlog(1, "%s no timestamp", url);
      goto cleanup;
    }

    sec_offset = offset_sec(remotetime, local_sec);
    if (sec_offset == LONG_MAX) {
      printlog(1, "%s unknown time format", url);
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
      printlog(0, "%-25s %s (%ld ms) => %ld", url, remotetime,
        rtt_ns / 1000000L, sec_offset);
    }

    steps--;
    when += nap;

    /* Stop if the RTT jitter exceeds the current bisection step size */
    double jitter = max_rtt - min_rtt;
    double step_size = (double)labs(nap) / NS_SEC;
    if (jitter > step_size) {
      if (verbose > 1)
        printlog(0, "%s RTT jitter %d ms exceeds step size %d ms",
          url, to_ms(jitter), to_ms(step_size));
      steps = 0;
    }
  } while (steps > 0);

  if (verbose > 1) printlog(0, "when: %ld, nap: %ld", when, nap);

  /* Rounding */
  *offset = -(double)first_offset + 1.0 - (double)when / (double)NS_SEC;

  /* Average RTT is only used to weight the servers in main() */
  *rtt = total_rtt / polls;

  ret = 0;

cleanup:
  if (list) curl_slist_free_all(list);
  if (curl) curl_easy_cleanup(curl);

  return ret;
}

static int calculate_offset(struct timesample samples[], int valid_samples,
  double *out) {
  double avgtime = 0.0;
  double total_weight = 0;

  if (valid_samples == 2 && dabs(samples[0].offset - samples[1].offset) > 0.5) {
    avgtime = (samples[0].rtt < samples[1].rtt) ? samples[0].offset : samples[1].offset;
    total_weight = 1;
  } else {
    double sorted_offsets[MAX_URLS];
    for (int j = 0; j < valid_samples; j++)
       sorted_offsets[j] = samples[j].offset;

    insertsort(sorted_offsets, valid_samples);
    double median_offset = sorted_offsets[valid_samples / 2];

    // Use Median Absolute Deviation (MAD) for outlier detection
    double devs[MAX_URLS];
    for (int j = 0; j < valid_samples; j++)
      devs[j] = dabs(samples[j].offset - median_offset);

    insertsort(devs, valid_samples);
    double mad = devs[valid_samples / 2];

    double threshold = 3.0 * mad;

    double weighted_sum = 0;

    for (int j = 0; j < valid_samples; j++) {
      if (dabs(samples[j].offset - median_offset) > threshold) {
        if (verbose)
          printlog(0, "Dropping outlier: offset %d ms from median",
          to_ms(samples[j].offset));
        continue;
      }

      // Inverse RTT weighted values
      double weight = 1.0 / (samples[j].rtt + 0.001);
      weighted_sum += samples[j].offset * weight;
      total_weight += weight;
    }

    if (total_weight > 0) {
      avgtime = weighted_sum / total_weight;
    }
  }

  if (total_weight <= 0)
    return -1;

  *out = avgtime;
  return 0;
}

static void show_help(void) {
  printf("httpdate version " VERSION "\n"
         "Usage: httpdate [-acdDFhnstvx] [-p #] [-P proxy] <URLs> ...\n\n"
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
         "  -t    disable time sanity check (limit: %ld days)\n"
         "  -v    show version\n"
         "  -x    adjust time and clock frequency (drift, use with -D)\n",
         DEFAULT_PRECISION, MAX_OFFSET / 86400L);
}

static int update_clock(double avgtime, int precision, int interval) {
  static time_t starttime = 0;

  // Consensus offset, median-filtered and RTT-weighted
  if (verbose || run_mode == RUN_ONESHOT)
    printlog(0, "Offset: %d ms", to_ms(avgtime));

  /* Consider +/- .5s a valid sync state */
  if (sync_mode == SYNC_MODE_DRIFT && dabs(avgtime) < 0.5)
    setstatus();

  /* Measurement resolution is +/- 1/2^precision seconds */
  double threshold = 1.0 / (double)(1 << precision);

  if (dabs(avgtime) <= threshold) {
    if (interval < DEFAULT_MAX_SLEEP) {
      interval++;
    }
    return interval;
  }

  /* Avoid bouncing between upper/lower limit when (almost) in sync */
  if (dabs(avgtime) < 1.0 && sync_mode != SYNC_MODE_SET) avgtime /= 2;

  if (sync_mode == SYNC_MODE_SET) setclock(avgtime);
  if (sync_mode == SYNC_MODE_ADJUST || sync_mode == SYNC_MODE_DRIFT) adjustclock(avgtime);

  if (run_mode != RUN_ONESHOT && sync_mode == SYNC_MODE_DRIFT) {
    if (starttime) {
      /* Calculate systematic clock drift */
      double elapsed = (double)(time(NULL) - starttime);
      double drift = avgtime / elapsed;
      printlog(0, "Drift %.2f PPM, %.2f s/day", drift * 1e6, drift * 86400);

      /* Adjust the clock frequency, weighted by elapsed time */
      if (adjust_freq(drift, (float)(elapsed / (elapsed + DRIFT_TAU))) < 0)
        printlog(1, "Frequency change failed");
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
  while ((param = getopt(argc, argv, "46acdhDFnp:P:stvx")) != -1)
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
        log_dest = LOG_DEST_SYSLOG;
        if (sync_mode != SYNC_MODE_DRIFT) sync_mode = SYNC_MODE_ADJUST;
        break;
      case 'F':
        run_mode = RUN_FOREGROUND;
        log_dest = LOG_DEST_CONSOLE;
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

  if (argc - optind > MAX_URLS) {
    printlog(1, "Too many URLs specified");
    exit(1);
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  switch (run_mode) {
  case RUN_ONESHOT:
    /* Run once without daemonizing or startup logging. */
    break;

  case RUN_BACKGROUND:
    openlog("httpdate", LOG_PID | LOG_CONS, LOG_DAEMON);

    if (daemon(0, 0) < 0) {
      perror("daemon initialization failed");
      curl_global_cleanup();
      exit(1);
    }

    printlog(0, "httpdate version "VERSION" started");
    break;

  case RUN_FOREGROUND:
    printlog(0, "httpdate version "VERSION" started");
    break;
  }

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
          printlog(0, "%-25s | Offset exceeds %ld days, ignored", argv[i], MAX_OFFSET / 86400L);
        continue;
      }

      if (verbose) printlog(0, "%-25s | Offset: %d ms | RTT: %d ms", argv[i], to_ms(offset), to_ms(rtt));
      samples[valid_samples].offset = offset;
      samples[valid_samples].rtt = rtt;
      valid_samples++;
    }

    if (valid_samples > 0) {
      if (calculate_offset(samples, valid_samples, &avgtime) == 0)
        have_time = 1;
      else
        printlog(1, "No servers passed RTT quality and consensus filters");
    } else {
      printlog(1, "No suitable servers available");
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
