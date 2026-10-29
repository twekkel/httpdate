#define _GNU_SOURCE

#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
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
#define DEFAULT_MAX_SLEEP       16              /* 2^16  ~18 hours   */
#define DEFAULT_PRECISION       3               /* 3  +/- 125 ms     */
#define DRIFT_TAU               43200.0         /* 12-hour smoothing */
#define PRINTBUFFERSIZE         1024
#define MAX_PPM                 2
#define MAX_URLS                16
#define NS_SEC                  1000000000L
#define MAX_DRIFT               32768000L       /* 500 PPM */
#define VERSION                 "1.0.0"

enum log_dest {
  LOG_DEST_CONSOLE,
  LOG_DEST_SYSLOG
};

enum run_mode {
  RUN_ONESHOT,
  RUN_BACKGROUND,
  RUN_FOREGROUND
};

enum sync_mode {
  SYNC_MODE_NONE,
  SYNC_MODE_SET,
  SYNC_MODE_ADJUST,
  SYNC_MODE_DRIFT
};

static enum log_dest log_dest = LOG_DEST_CONSOLE;
static enum run_mode run_mode = RUN_ONESHOT;
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

static long offset_sec(char remotetime[21]) {
  struct timespec now;
  struct tm tm = {0};

  clock_gettime(CLOCK_REALTIME, &now);

  if (strptime(remotetime, "%d %b %Y %H:%M:%S", &tm) == NULL) {
    return LONG_MAX;
  }

  time_t remote_epoch = timegm(&tm);
  if (remote_epoch == (time_t)-1) {
    return LONG_MAX;
  }

  return now.tv_sec - (long)remote_epoch;
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

  printlog(0, "Adjusting %d ms", (int)round(timedelta * 1000.0));

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
  *list = curl_slist_append(*list, "Pragma: no-cache");
  *list = curl_slist_append(*list, "Cache-Control: no-cache");
  *list = curl_slist_append(*list, "Connection: keep-alive");

  if (proxy)
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *list);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, getremotetime);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)remotetime);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, ip_resolve);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "httpdate/"VERSION);

  if (verbose > 2) {
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }

  return CURLE_OK;
}

static double bisect(const char *url, int precision, double *out_rtt) {
  CURL *curl = NULL;
  struct curl_slist *list = NULL;
  CURLcode res;
  struct timespec now, sleepspec;
  char remotetime[25] = {0};
  double result = INFINITY;
  double last_rtt = 0.0;
  double total_rtt = 0.0;
  double avg_rtt = 0.0;
  long offset = 0, first_offset = 0, prev_offset = 0;
  long nap = NS_SEC;
  long rtt = 0;
  long when = nap >> precision;
  long target;
  int polls = 0;

  curl = curl_easy_init();
  if (!curl) {
    return INFINITY;
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
  rtt = getrtt(curl, &last_rtt);

  do {
    if (verbose > 1)
      printlog(0, "bisect: %i, when: %09li", polls, when);

    /* Initialize timer */
    clock_gettime(CLOCK_REALTIME, &now);

    /* Wait till we reach the desired time, "when" - latency */
    target = when - rtt / 2;
    if (target < 0) target += NS_SEC;

    sleepspec.tv_sec = 0;
    if (target >= now.tv_nsec) {
      sleepspec.tv_nsec = target - now.tv_nsec;
    } else {
      sleepspec.tv_nsec = NS_SEC + target - now.tv_nsec;
    }
    nanosleep(&sleepspec, NULL);

    /* Send HEAD request */
    remotetime[0] = '\0';
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      printlog(1, "failed: %s %s", url, curl_easy_strerror(res));
      goto cleanup;
    }

    /* rtt contains round trip time in nanoseconds */
    rtt = getrtt(curl, &last_rtt);

    if (remotetime[0] == '\0') {
      printlog(1, "%s no timestamp", url);
      goto cleanup;
    }

    offset = offset_sec(remotetime);
    if (offset == LONG_MAX) {
      printlog(1, "%s unknown time format", url);
      goto cleanup;
    }

    polls++;
    total_rtt += last_rtt;

    nap >>= 1;
    if (polls > 1) {
      if (offset != prev_offset) nap = -nap;
    } else {
      first_offset = offset;
    }
    prev_offset = offset;

    if (verbose > 1) {
      printlog(0, "%-25s %s (%ld ms) => %ld", url, remotetime,
        rtt / 1000000L, offset);
    }

    precision--;
    when += nap;
  } while (precision > 0);

  if (verbose > 1) printlog(0, "when: %ld, nap: %ld", when, nap);

  /* Rounding */
  result = -(double)first_offset + 1.0 - (double)when / (double)NS_SEC;

  /* Average RTT is only used to weight the servers in main() */
  avg_rtt = total_rtt / polls;

cleanup:
  if (list) curl_slist_free_all(list);
  if (curl) curl_easy_cleanup(curl);

  if (out_rtt && result != INFINITY) {
    *out_rtt = avg_rtt;
  }

  return result;
}

static double calculate_offset(struct timesample samples[], int valid_samples) {
  double avgtime = INFINITY;
  double total_weight = 0;

  if (valid_samples == 2 && fabs(samples[0].offset - samples[1].offset) > 0.5) {
    avgtime = (samples[0].rtt < samples[1].rtt) ? samples[0].offset : samples[1].offset;
    total_weight = 1;
  } else {
    double sorted_offsets[MAX_URLS];
    for (int j = 0; j < valid_samples; j++)
       sorted_offsets[j] = samples[j].offset;

    insertsort(sorted_offsets, valid_samples);

    double median_offset = sorted_offsets[valid_samples / 2];

    double weighted_sum = 0;

    /* Filter out the bogus timevalues. An offset which is more than
       1 second off from mean, is considered a 'false ticker'.
       NTP synced web servers can never be more off than a second.
    */
    for (int j = 0; j < valid_samples; j++) {
      if (fabs(samples[j].offset - median_offset) > 0.5) {
        if (verbose)
          printlog(0, "Dropping outlier: offset %d ms from median",
          (int)round(samples[j].offset * 1000.0));
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

  return (total_weight > 0) ? avgtime : INFINITY;
}

static void show_help(void) {
  printf("httpdate version " VERSION "\n"
         "Usage: httpdate [-adDFhnsvx] [-p #] [-P proxy] <URLs> ...\n\n"
         "  -4    force IPv4 resolution\n"
         "  -6    force IPv6 resolution\n"
         "  -a    adjust time gradually\n"
         "  -d    debug/verbose output\n"
         "  -D    run as daemon\n"
         "  -F    run daemon in foreground\n"
         "  -h    help\n"
         "  -n    ignore proxy environment variables\n"
         "  -p    precision (1..9, default: %d)\n"
         "  -P    use proxy server (e.g., http://proxy:port or host:port)\n"
         "  -s    set time\n"
         "  -v    show version\n"
         "  -x    adjust time and clock frequency (drift, use with -D)\n",
         DEFAULT_PRECISION);
}

static void update_clock(double avgtime, int precision, int *interval,
  time_t *starttime) {

  // Consensus offset, median-filtered and RTT-weighted
  if (verbose || run_mode == RUN_ONESHOT)
    printlog(0, "Offset: %d ms", (int)round(avgtime * 1000.0));

  /* Consider +/- .5s a valid sync state */
  if (sync_mode == SYNC_MODE_DRIFT && fabs(avgtime) < 0.5)
    setstatus();

  /* Measurement resolution is +/- 1/2^precision seconds */
  double threshold = 1.0 / (double)(1 << precision);

  if (fabs(avgtime) <= threshold) {
    if (*interval < DEFAULT_MAX_SLEEP) {
      (*interval)++;
    }
    return;
  }

  /* Avoid bouncing between upper/lower limit when (almost) in sync */
  if (fabs(avgtime) < 1.0 && sync_mode != SYNC_MODE_SET) avgtime /= 2;

  if (sync_mode == SYNC_MODE_SET) setclock(avgtime);
  if (sync_mode == SYNC_MODE_ADJUST || sync_mode == SYNC_MODE_DRIFT) adjustclock(avgtime);

  if (run_mode != RUN_ONESHOT && sync_mode == SYNC_MODE_DRIFT) {
    if (*starttime) {
      /* Calculate systematic clock drift */
      double elapsed = (double)(time(NULL) - *starttime);
      double drift = avgtime / elapsed;
      printlog(0, "Drift %.2f PPM, %.2f s/day", drift * 1e6, drift * 86400);

      /* Adjust the clock frequency, weighted by elapsed time */
      if (adjust_freq(drift, (float)(elapsed / (elapsed + DRIFT_TAU))) < 0)
        printlog(1, "Frequency change failed");
    }
    *starttime = time(NULL);
  }

  /* Only force DEFAULT_MIN_SLEEP for large corrections,
     otherwise ease back off
  */
  if (fabs(avgtime) > (threshold * 2.0)) {
    *interval = DEFAULT_MIN_SLEEP;
  } else if (*interval > DEFAULT_MIN_SLEEP) {
    (*interval)--;
  }
}

int main(int argc, char *argv[]) {
  int       interval   = DEFAULT_MIN_SLEEP;
  int       precision  = DEFAULT_PRECISION;
  time_t    starttime  = 0;

  int       param;
  while ((param = getopt(argc, argv, "46adhDFnp:P:svx")) != -1)
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
      case 'v':
        printf("httpdate version %s\n", VERSION);
        exit(0);
      case 'x':
        sync_mode = SYNC_MODE_DRIFT;
        if (precision < 7) precision = 7;
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
    double avgtime = INFINITY;

    for (int i = optind; i < argc; i++) {
      double offset;
      double rtt = 0.0;

      offset = bisect(argv[i], precision, &rtt);
      if (offset == INFINITY)
        continue;

      if (verbose) printlog(0, "%-25s | Offset: %d ms | RTT: %d ms", argv[i], (int)round(offset * 1000.0), (int)round(rtt * 1000.0));
      samples[valid_samples].offset = offset;
      samples[valid_samples].rtt = rtt;
      valid_samples++;
    }

    if (valid_samples > 0) {
      avgtime = calculate_offset(samples, valid_samples);
      if (avgtime == INFINITY)
        printlog(1, "No servers passed RTT quality and consensus filters");
    } else {
      printlog(1, "No suitable servers available");
    }

    if (avgtime != INFINITY) {
      update_clock(avgtime, precision, &interval, &starttime);
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
