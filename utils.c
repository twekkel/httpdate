/* util.c */
#include <stdio.h>
#include <string.h>
#include "utils.h"
 
int yisleap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int yday(int mon, int day, int year) {
  static const int days[2][13] = {
    {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
    {0, 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335}
  };
  int leap = yisleap(year);

  return days[leap][mon] + day;
}

long epoch(int day, int month, int year, int hour, int minute, int second){

  return (
    second + minute * 60 + hour * 3600 + 
    yday(month + 1, day - 1, year) * 86400 +
    (year-70)*31536000 + ((year-69)/4)*86400 -
    ((year-1)/100)*86400 + ((year+299)/400)*86400);
}

int month_index(const char *month_name) {
  char monthString[36] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char *s = NULL;

  s = strstr(monthString, month_name);
  if (s != NULL) {
    return (s - monthString) / 3;
  } else {
    return -1;
  }
}

void insertsort(double a[], long length) {
  long   i, j;

  for (i = 1; i < length; i++) {
    double value = a[i];
    for (j = i - 1; j >= 0 && a[j] > value; j--)
      a[j+1] = a[j];
    a[j+1] = value;
  }
}
