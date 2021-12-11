### Short

```
date -s "`curl --head -s https://example.com | grep -i "Date: " | cut -d' ' -f2-`"
```

### Longer... httpdate

The above one-liner might give unexpected results when,
* site is not reachable
* site has wrong time
* steps/jumps (backwards!) in time

httpdate solves that by allowing multiple URLs as time source, eliminating 'false tickers' and gradually adjusting time.

HTTP, HTTPS, proxies servers are all supported, thanks to [libcurl](https://curl.se/libcurl/).

### Install

Make sure libcurl-dev/libcurl4-gnutls-dev (or similar) libraries is installed.

```
make
make install
```
Setting and or adjusting time requires root privileges.

### Usage

```
Usage: httpdate [-adhs] [-p #] <URL> ...

  -a    adjust time gradually
  -d    debug/verbose output
  -h    help
  -p    precision
  -s    set time
  -v    show version
```

Httpdate tries to approximate the 'second boundary'. With every request it moves closer to that boundary, by default in 6 steps.

ntpdate versus httpdate

```
httpdate -p 10 xs4all.nl
Offset: -0.150 s

ntpdate -q ntp.xs4all.nl
server 2001:888:0:7::2, stratum 2, offset -0.153365, delay 0.02866
server 194.109.6.2, stratum 2, offset -0.153384, delay 0.02867
11 Dec 15:09:19 ntpdate[14535]: adjust time server 2001:888:0:7::2 offset -0.153365 sec
```

### See also

* https://github.com/twekkel/htpdate - daemon using similar idea
* https://github.com/angeloc/htpdate - fork of htpdate, with HTTPS support
