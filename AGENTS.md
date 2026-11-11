# AGENTS.md

httpdate: time synchronization over HTTP(S), C11 with libcurl, optional minimal NTP server (`-S`).

## Principles

- Small changes preferred, no bloat
- Smallest diff that solves the problem; no drive-by refactors or reformatting
- No new dependencies (libcurl only), no threads
- Prefer int/long over float/double; new code uses integer ms/ns (existing double code stays)

## Layout

- `httpdate.c`: main, option parsing, polling, clock updates
- `ntpd.c`, `ntpd.h`: NTP server, runs in a forked child (`WITH_NTP`)
- `utils.c`, `utils.h`: small helpers (parsing, sorting, rounding)
- `log.c`, `log.h`: console/syslog logging
- `httpdate.8`, `README.md`: documentation, `Makefile`: build

## Style

- 2 spaces indent, no tabs, no trailing whitespace, ASCII only (also in comments)
- `// short comment`; explain why, not what
- snake_case functions and variables, UPPER_CASE macros
- No braces around a single statement; `switch (x)` and `if (x)` with a space
- One blank line between functions, never two
- Keep lines readable, wrap long calls
- Include order: `#define _DEFAULT_SOURCE`, blank line, own headers, blank line,
  system headers in alphabetical order. Exception: `<curl/curl.h>` and its
  version check come first in `httpdate.c`
- Header guards end with `#endif // NAME_H`

## Comments

- The header documents the contract: what it does, arguments, return value, side effects
- The `.c` file documents how and why: workarounds, assumptions, invariants
- Never repeat the same comment in both; `static` functions are commented in the `.c`

## Code rules

- Signal handlers: async-signal-safe calls only; set a flag and log from the main loop
- Errors: `print_log(1, ...)`; one-shot mode exits 1 on failure, daemon mode retries
- Keep the offset sanity check (`MAX_OFFSET`, `-t`) intact
- NTP server handles untrusted packets: no allocation, no per-client state,
  fixed 48-byte response, drop anything malformed
- Stay portable: Linux, FreeBSD, NetBSD (`adjtimex` alias for the BSDs)

## When changing behavior

- New or changed option: update `show_help()`, the `getopt` string, `README.md` and `httpdate.8` together
- Keep the option order identical in `show_help()`, the `getopt` string and the docs
- Do not bump `VERSION` unless asked

## Checks

- `make` builds without warnings (`-Wall -std=c11 -pedantic`)
- `make clean && make WITH_NTP=0` also builds
- `make test` queries the network; run it only when network access is available
