#ifndef UTILS_H
#define UTILS_H

// Parse n decimal digits at p; -1 on error
int num(const char *p, int n);

// "DD Mon YYYY HH:MM:SS" (UTC) -> Unix time, or -1 on error
long long parse_imf_fixdate(const char *s);

// Insertion sort is more efficient (and smaller) than qsort for small lists
void insert_sort(double a[], int length);

// fabs equivalent, without needing math.h
double dabs(double x);

// Seconds -> rounded milliseconds
long long to_ms(double sec);

#endif
