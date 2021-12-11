#define _GNU_SOURCE

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "utils.h"

#define VERSION                 "0.0.1"
#define MAXURLS                 16
#define DEFAULT_PRECISION       6
#define MAXHEADERSIZE           8192

static int verbose = 0;

static size_t getremotetime(char buffer[MAXHEADERSIZE], size_t size,
                     size_t nmemb, char remotetime[25]) {
  char *pdate = NULL;

  if ((pdate = strcasestr(buffer, "date: ")) != NULL && strlen(pdate) >= 35) {
    strncpy(remotetime, pdate + 11, 24);
    if (verbose) printf("%s, ", remotetime);
  }
  return size*nmemb;
}

static int offset_sec(char remotetime[25]) {
  struct timeval    timevalue       = {LONG_MAX, 0};
  struct timespec   now;
  char   month[3];
  int    day, year, hour, minute, second;

  clock_gettime(CLOCK_REALTIME, &now);
  sscanf(remotetime, "%d %s %d %d:%d:%d GMT", &day, month, &year, &hour, &minute, &second);
  timevalue.tv_sec = epoch(day, month_index(month), year - 1900, hour, minute, second);
  if (verbose) printf("%li, ", now.tv_sec - timevalue.tv_sec);
  return now.tv_sec - timevalue.tv_sec;
}

static int setclock(double timedelta) {
  struct timespec now;
  char   buffer[32] = {'\0'};

  clock_gettime(CLOCK_REALTIME, &now);
  timedelta += (now.tv_sec + now.tv_nsec * 1e-9);

  now.tv_sec  = (long)timedelta;
  now.tv_nsec = (long)(timedelta - now.tv_sec) * 1e9;

  strftime(buffer, sizeof(buffer), "%c", localtime(&now.tv_sec));
  printf("Set time: %s\n", buffer);

  if (clock_settime(CLOCK_REALTIME, &now)) {
    fputs("error setting time\n", stderr);
    exit(1);
  }
  return(clock_settime(CLOCK_REALTIME, &now));
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

static double bisect(char url[128], int precision) {
  CURL      *curl;
  CURLcode  res;
  struct    timespec   timeofday;
  char      remotetime[25] = {'\0'};
  struct    timespec     sleepspec;
  long      first_offset=0;
  int       polls = 0;
  long      when = 0;

  curl = curl_easy_init();
  if(curl) {
    struct curl_slist *list = NULL;
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
    curl_easy_setopt(curl, CURLOPT_HEADER, 1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "httpdate/"VERSION);
    list = curl_slist_append(list, "Connection: keep-alive");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    if (verbose) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, getremotetime);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &remotetime);

    // Send message in advance to prevent DNS delay
    res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
      fprintf(stderr, "failed: %s %s\n", url, curl_easy_strerror(res));
      curl_easy_cleanup(curl);
      return(LONG_MAX);
    }

    long offset  = 0;
    long prev_offset = 0;
    long subsec = 1e9L;
    when = 0;
    do {
      clock_gettime( CLOCK_REALTIME, &timeofday);
      if (verbose) printf("when: %ld now: %ld\n", when, timeofday.tv_nsec);
      sleepspec.tv_sec = 0;
      if (when >= timeofday.tv_nsec) {
        sleepspec.tv_nsec = when - timeofday.tv_nsec;
      } else {
        sleepspec.tv_nsec = 1e9L + when - timeofday.tv_nsec;
      }
      nanosleep(&sleepspec, NULL);

      res = curl_easy_perform(curl);
      if(res != CURLE_OK) {
        fprintf(stderr, "failed: %s %s\n", url, curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return(LONG_MAX);
      } else {
        polls++;
        offset = offset_sec(remotetime);
        if (verbose) printf("%s\n", url);

        subsec >>= 1;
        if (polls>1) {
          if (offset != prev_offset) subsec = -subsec;
        } else {
          first_offset=offset;
        }
        prev_offset=offset;
      }
      precision --;
      when += subsec;
    } while (precision > 1);         /* do */
  }
  curl_easy_cleanup(curl);

  if (first_offset < 0) {
    return(-first_offset + (1000000000L-when)/(double)1000000000L);
  } else {
    return(-first_offset + 1 - when/(double)1000000000L);
  }
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
  double    timedeltas[MAXURLS-1];
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
        verbose = 1;
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

  for (i = optind; i < argc; i++) {
    offset = bisect(argv[i], precision);
    if (verbose) printf("%s\n", argv[i]);
    timedeltas[validtimes] = offset;
    validtimes++;
  }

  insertsort(timedeltas, validtimes);
  mean = timedeltas[validtimes/2];

  for (int j = 0; j < validtimes; j++) {
    if (abs(timedeltas[j]-mean) <= 1) {
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
  if (setmode == 1) setclock((double)offset);
  if (setmode == 2) adjustclock((double)offset);

  printf("Offset: %.3f s\n", avgtime);

  return 0;
}
