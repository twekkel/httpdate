#define _GNU_SOURCE

#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "utils.h"

#define VERSION                 "0.0.1"
#define MAXURLS                 16
#define DEFAULT_PRECISION       6
#define MAXHEADERSIZE           8192

static int verbose = 0;

static size_t getremotetime(void *contents, size_t size,
  size_t nmemb, void *userdata) {
  const char *buffer = contents;
  char *remotetime = userdata;
  const char *pdate;
  size_t len = size * nmemb;

  pdate = strcasestr(buffer, "date: ");
  if (pdate) {
    memcpy(remotetime, pdate + 11, 24);
    remotetime[24] = '\0';

    if (verbose)
      printf("%s", remotetime);
  }

  return len;
}

static long int offset_sec(char remotetime[25]) {
  struct timeval    timevalue       = {LONG_MAX, 0};
  struct timespec   now;
  char   month[4];
  int    day, year, hour, minute, second;

  clock_gettime(CLOCK_REALTIME_COARSE, &now);
  if (sscanf(remotetime, "%d %3s %d %d:%d:%d GMT",
    &day, month, &year, &hour, &minute, &second) != 6)
  {
    return LONG_MAX;
  }
  timevalue.tv_sec = epoch(day, month_index(month), year - 1900, hour, minute, second);
  if (verbose) printf("%li, ", now.tv_sec - timevalue.tv_sec);
  return now.tv_sec - timevalue.tv_sec;
}

static int setclock(double timedelta) {
  struct timespec now;
  char   buffer[32] = {'\0'};

  clock_gettime(CLOCK_REALTIME_COARSE, &now);
  timedelta += (now.tv_sec + now.tv_nsec * 1e-9);

  now.tv_sec  = (long)timedelta;
  now.tv_nsec = (long)((timedelta - now.tv_sec) * 1e9);

  strftime(buffer, sizeof(buffer), "%c", localtime(&now.tv_sec));
  printf("Set time: %s\n", buffer);

  int ret = clock_settime(CLOCK_REALTIME, &now);

  if (ret) {
    fputs("error setting time\n", stderr);
    exit(1);
  }

  return ret;
}

static int adjustclock(double timedelta) {
  struct timeval    timeofday;
  int               ret;

  printf("Adjusting %.3f seconds\n", timedelta);

  timeofday.tv_sec  = (long)timedelta;
  timeofday.tv_usec = (long)((timedelta - timeofday.tv_sec) * 1e6L);

  ret = adjtime(&timeofday, NULL);
  return(ret);
}

static double bisect(const char *url, int precision) {
  CURL *curl = NULL;
  struct curl_slist *list = NULL;
  CURLcode res;
  struct timespec timeofday;
  char remotetime[25] = {0};
  struct timespec sleepspec;
  long first_offset = 0;
  int polls = 0;
  long when = 0;
  double result = LONG_MAX;

  curl = curl_easy_init();
  if (!curl) {
    return LONG_MAX;
  }

  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "httpdate/"VERSION);
  
  list = curl_slist_append(list, "Connection: keep-alive");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  if (verbose > 1) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(curl, CURLOPT_URL, url);

  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, getremotetime);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)remotetime);

  // Send message in advance to prevent DNS delay
  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "failed: %s %s\n", url, curl_easy_strerror(res));
    goto cleanup;
  }
  if (verbose) puts("");

  long offset = 0;
  long prev_offset = 0;
  long subsec = 1e9L;
  when = 0;
  do {
    clock_gettime(CLOCK_REALTIME_COARSE, &timeofday);
    if (verbose) printf("when: %09ld now: %ld\n", when, timeofday.tv_nsec);
    sleepspec.tv_sec = 0;
    if (when >= timeofday.tv_nsec) {
      sleepspec.tv_nsec = when - timeofday.tv_nsec;
    } else {
      sleepspec.tv_nsec = 1e9L + when - timeofday.tv_nsec;
    }
    nanosleep(&sleepspec, NULL);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      fprintf(stderr, "failed: %s %s\n", url, curl_easy_strerror(res));
      goto cleanup;
    } else {
      polls++;
      offset = offset_sec(remotetime);
      if (verbose) printf(", %s\n", url);

      subsec >>= 1;
      if (polls > 1) {
        if (offset != prev_offset) subsec = -subsec;
      } else {
        first_offset = offset;
      }
      prev_offset = offset;
    }
    precision--;
    when += subsec;
  } while (precision > 1);

  if (first_offset < 0) {
    result = -first_offset + (1000000000L - when) / (double)1000000000L;
  } else {
    result = -first_offset + 1 - when / (double)1000000000L;
  }

cleanup:
  // Single, centralized cleanup block guarantees no leaks
  if (list) curl_slist_free_all(list);
  if (curl) curl_easy_cleanup(curl);
  return result;
}

static void showhelp() {
  puts("httpdate version "VERSION"\n\
Usage: httpdate [-adhs] [-p #] <URL> ...\n\n\
  -a    adjust time gradually\n\
  -d    debug/verbose output\n\
  -h    help\n\
  -p    precision\n\
  -s    set time\n\
  -v    show version\n");
  return;
}

int main(int argc, char *argv[]) {
  int       precision = DEFAULT_PRECISION;
  long      setmode    = 0 ;
  double    sumtimes   = 0;
  double    timedeltas[MAXURLS];
  double    offset = 0, mean = 0, avgtime;
  int       validtimes = 0, goodtimes = 0;
  int       param;
  int       i;

  while ((param = getopt(argc, argv, "adhp:sv")) != -1)
    switch(param) {
      case 'a':
        setmode = 2;
        break;
      case 'd':
        if (verbose < 2) verbose++;
        break;
      case 'h':
        showhelp();
        exit(0);
      case 'p':
        precision = atoi(optarg);
        break;
      case 's':
        setmode = 1;
        break;
      case 'v':
        printf("httpdate version %s\n", VERSION);
        exit(0);
      case '?':
        return 1;
      default:
        abort();
    }

  if (argv[optind] == NULL) {
    showhelp();
    exit(1);
  }

  if (argc - optind > MAXURLS) {
    fputs("Too many URLs specified\n", stderr);
    exit(1);
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  for (i = optind; i < argc; i++) {
    offset = bisect(argv[i], precision);
    if (verbose) printf("%s\n", argv[i]);
    timedeltas[validtimes] = offset;
    validtimes++;
  }

  insertsort(timedeltas, validtimes);
  mean = timedeltas[validtimes/2];

  for (int j = 0; j < validtimes; j++) {
    if (fabs(timedeltas[j]-mean) <= 1.0) {
      sumtimes += timedeltas[j];
      goodtimes++;
    }
  }

  if (goodtimes) {
    avgtime = sumtimes / (double)goodtimes;
    if (verbose) printf("Average offset: %.3f\n", avgtime);
  } else {
    fputs("No suitable servers\n", stderr);
    exit(1);
  }
  if (setmode == 1) setclock((double)avgtime);
  if (setmode == 2) adjustclock((double)avgtime);

  printf("Offset: %.3f s\n", avgtime);

  curl_global_cleanup();
  return 0;
}
