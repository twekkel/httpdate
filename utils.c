#include "utils.h"
#include <string.h>

int num(const char *p, int n) {
  int v = 0;
  while (n--) {
    unsigned d = (unsigned char)*p++ - '0';
    if (d > 9) return -1;
    v = v * 10 + (int)d;
  }
  return v;
}

long long parse_imf_fixdate(const char *s) {
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

void insert_sort(double a[], int length) {
  for (int i = 1; i < length; i++) {
    double value = a[i];
    int j;

    for (j = i - 1; j >= 0 && a[j] > value; j--) {
      a[j + 1] = a[j];
    }
    a[j + 1] = value;
  }
}

double dabs(double x) {
  return x < 0 ? -x : x;
}

long long to_ms(double sec) {
  return (long long)(sec * 1000.0 + (sec < 0 ? -0.5 : 0.5));
}
