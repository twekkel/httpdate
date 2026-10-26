#define _GNU_SOURCE

#include <curl/curl.h>
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
#define PRINTBUFFERSIZE         1024
#define MAXURLS                 16
#define NS_SEC                  1000000000L
#define MAX_DRIFT               32768000        /* 500 PPM */
#define VERSION                 "1.0.0"

static int logmode     = 0;
static char *proxy_url = NULL;
static int verbose     = 0;

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

  switch(logmode) {
    case 0:
      fprintf(is_error ? stderr : stdout, "%s\n", buf);
      fflush(is_error ? stderr : stdout);
      break;
    case 1:
      syslog(is_error ? LOG_WARNING : LOG_INFO, "%s", buf);
      break;
    case 2:
      fprintf(stderr, "%s\n", buf);
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

static int adjustfreq(double drift, float confidence) {
  struct timex tmx = {0};

  /* Read current clock frequency */
  tmx.modes = 0;
  adjtimex(&tmx);

  /* Weighted average of current and new frequency */
  tmx.freq = tmx.freq + (long int)(65536e6 * drift * confidence);
  if ((tmx.freq < -MAX_DRIFT) || (tmx.freq > MAX_DRIFT))
    tmx.freq = (tmx.freq < 0 ? -1 : 1) * MAX_DRIFT;

  printlog(0, "Set frequency %li", tmx.freq);
  tmx.modes = MOD_FREQUENCY;

  return(adjtimex(&tmx));
}

/* Same bisection as htpdate's getHTTPdate(): the requests are sent
   'latency' (= rtt / 2) ahead of time, so they hit the web server at the
   sub-second position "when". No RTT correction is needed afterwards.
*/
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
  long latency = 0;
  long rtt = 0;
  long when = nap >> precision;
  long target;
  int polls = 0;

  curl = curl_easy_init();
  if (!curl) {
    return INFINITY;
  }

  /* Pragma: no-cache, "forces" an HTTP/1.0 and 1.1 compliant
     web server to return a fresh timestamp
     Connection: keep-alive, for multiple requests
  */
  list = curl_slist_append(list, "Pragma: no-cache");
  list = curl_slist_append(list, "Cache-Control: no-cache");
  list = curl_slist_append(list, "Connection: keep-alive");

  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "httpdate/"VERSION);
  if (proxy_url)
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url);
  curl_easy_setopt(curl, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, getremotetime);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)remotetime);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);

  if (verbose > 2) {
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }

  /* Connect (DNS, TCP, TLS) before the poll loop. The CURLINFO_TOTAL_TIME
     of the polls below is then a plain request/response time,
     without connection setup. The result of this request is discarded.
  */
  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    printlog(1, "failed: %s %s", url, curl_easy_strerror(res));
    goto cleanup;
  }

  do {
    if (verbose > 1)
      printlog(0, "bisect: %i, when: %09li", polls, when);

    /* Initialize timer */
    clock_gettime(CLOCK_REALTIME, &now);

    /* Wait till we reach the desired time, "when" - latency */
    target = when - latency;
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
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &last_rtt);
    rtt = (long)(last_rtt * 1e9);
    if (rtt < 2) rtt = 2;               /* latency must never stay 0 */

    /* Obtain rtt/latency first */
    if (latency == 0) {
      latency = rtt / 2;
      continue;
    }
    latency = rtt / 2;

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
  if (when + nap == NS_SEC && offset == 0) {
    result = 0.0;
  } else if (first_offset < 0) {
    result = -(double)first_offset + ((double)NS_SEC - (double)when) / (double)NS_SEC;
  } else {
    result = -(double)first_offset + 1.0 - (double)when / (double)NS_SEC;
  }

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

static void showhelp(void) {
  printf("httpdate version " VERSION "\n"
         "Usage: httpdate [-adDFhsvx] [-p #] [-P proxy] <URLs> ...\n\n"
         "  -a    adjust time gradually\n"
         "  -d    debug/verbose output\n"
         "  -D    run as daemon\n"
         "  -F    run daemon in foreground\n"
         "  -h    help\n"
         "  -p    precision (1..9, default: %d)\n"
         "  -P    use proxy server (e.g., http://proxy:port or host:port)\n"
         "  -s    set time\n"
         "  -v    show version\n"
         "  -x    adjust time and clock frequency (drift, use with -D)\n",
         DEFAULT_PRECISION);
}

int main(int argc, char *argv[]) {
  int       setmode    = 0;
  int       daemonize  = 0;
  int       foreground = 0;
  int       interval   = DEFAULT_MIN_SLEEP;
  int       precision  = DEFAULT_PRECISION;
  double    drift      = 0;
  time_t    starttime  = 0;

  int       param;
  while ((param = getopt(argc, argv, "adhDFp:P:svx")) != -1)
    switch(param) {
      case 'a':
        setmode = 2;
        break;
      case 'd':
        if (verbose < 3) verbose++;
        break;
      case 'D':
        daemonize = 1;
        logmode = 1;
        if (setmode != 3) setmode = 2;
        break;
      case 'F':
        daemonize = 1;
        foreground = 1;
        logmode = 0;
        if (setmode != 3) setmode = 2;
        break;
      case 'h':
        showhelp();
        exit(0);
      case 'p':
        precision = atoi(optarg);
        if (precision < 1 || precision > 9) {
          fprintf(stderr, "Error: precision must be between 1 and 9\n");
          exit(1);
        }
        break;
      case 'P':
        proxy_url = optarg;
        break;
      case 's':
        setmode = 1;
        break;
      case 'v':
        printf("httpdate version %s\n", VERSION);
        exit(0);
      case 'x':               /* adjust time and clock frequency */
        setmode = 3;
        if (precision < 7) precision = 7;
        break;
      case '?':
      default:
        showhelp();
        exit(1);
    }

  if (argv[optind] == NULL) {
    showhelp();
    exit(1);
  }

  if (argc - optind > MAXURLS) {
    printlog(1, "Too many URLs specified");
    exit(1);
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  if (daemonize) {
    if (!foreground) {
      openlog("httpdate", LOG_PID | LOG_CONS, LOG_DAEMON);
      if (daemon(0, 0) < 0) {
        perror("daemon initialization failed");
        curl_global_cleanup();
        exit(1);
      }
    }
    printlog(0, "httpdate version "VERSION" started");
  }

  do {
    struct timesample samples[MAXURLS];
    int valid_samples = 0;
    double avgtime = 0;

    for (int i = optind; i < argc; i++) {
      double offset;
      double rtt = 0.0;

      offset = bisect(argv[i], precision, &rtt);
      if (offset != INFINITY) {
        if (verbose) printlog(0, "%-25s | Offset: %d ms | RTT: %d ms", argv[i], (int)round(offset * 1000.0), (int)round(rtt * 1000.0));
        samples[valid_samples].offset = offset;
        samples[valid_samples].rtt = rtt;
        valid_samples++;
      }
    }

    if (valid_samples > 0) {
      double total_weight = 0;

      if (valid_samples == 2 && fabs(samples[0].offset - samples[1].offset) > 0.5) {
        avgtime = (samples[0].rtt < samples[1].rtt) ? samples[0].offset : samples[1].offset;
        total_weight = 1;
      } else {
        double sorted_offsets[MAXURLS];
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

      if (total_weight > 0) {
        // Consensus offset, median-filtered and RTT-weighted
        if (verbose || !daemonize)
          printlog(0, "Offset: %d ms", (int)round(avgtime * 1000.0));

        /* Consider +/- .5s a valid sync state */
        if (setmode == 3 && fabs(avgtime) < 0.5)
          setstatus();

        /* Measurement resolution is +/- 1/2^precision seconds */
        double threshold = 1.0 / (double)(1 << precision);

        if (fabs(avgtime) > threshold) {
          /* Avoid bouncing between upper/lower limit when (almost) in sync */
          if (fabs(avgtime) < 1.0 && setmode != 1) avgtime /= 2;

          if (setmode == 1) setclock(avgtime);
          if (setmode == 2 || setmode == 3) adjustclock(avgtime);

          if (daemonize && setmode == 3) {
            if (starttime) {
              /* Calculate systematic clock drift */
              double elapsed = (double)(time(NULL) - starttime);
              drift = avgtime / elapsed;
              printlog(0, "Drift %.2f PPM, %.2f s/day", drift * 1e6, drift * 86400);

              /* Adjust the clock frequency, weighted by elapsed time */
              if (adjustfreq(drift, (float)(elapsed / (elapsed + 3600.0))) < 0)
                printlog(1, "Frequency change failed");
            }
            starttime = time(NULL);
          }

          interval = DEFAULT_MIN_SLEEP;

          /* Let adjtime() finish before the next poll cycle */
          if (daemonize && setmode == 3) sleep((unsigned int)fabs(avgtime * 2000));
        } else {
          if (interval < DEFAULT_MAX_SLEEP) {
             interval++;
          }
        }
      } else {
        printlog(1, "No servers passed RTT quality and consensus filters");
        if (!daemonize) {
          curl_global_cleanup();
          exit(1);
        }
      }
    } else {
      printlog(1, "No suitable servers available");
      if (!daemonize) {
        curl_global_cleanup();
        exit(1);
      }
    }

    if (daemonize) {
      sleep(1<<interval);
    }

  } while (daemonize);

  curl_global_cleanup();
  return 0;
}
