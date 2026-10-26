# httpdate

[![main](https://github.com/twekkel/httpdate/actions/workflows/main.yml/badge.svg)](https://github.com/twekkel/httpdate/actions/workflows/main.yml)

`httpdate` is the modern successor to [`htpdate`](https://github.com/twekkel/htpdate). Like its predecessor, it synchronizes time using HTTP date headers, making it useful on networks where NTP is blocked but web access through HTTP or HTTPS proxies is permitted. Built on `libcurl`, it supports HTTP, HTTPS, HTTP/2, and proxy servers (including authentication). It refines time estimation through multi-server consensus, outlier filtering, and round-trip-time weighting, and includes optional clock-frequency drift compensation for continuous daemon operation.

## Requirements

- A C compiler and development tools
- libcurl development headers and library
- Network access to the configured HTTP/HTTPS servers

Accuracy depends on server quality, network latency, and the precision setting. If you need a more established time-synchronization solution, consider `chrony` or `ntpd`.

## Build and install

The exact package names for the compiler and libcurl development files depend on your operating system.

### Dependencies

On Debian/Ubuntu systems, install the required build tools and libcurl development package:

```sh
sudo apt update
sudo apt install build-essential libcurl4-openssl-dev
```

### Compile, test, and install `httpdate`

```sh
make
make test
sudo make install
```

Check the installed version and command-line options:

```sh
/usr/local/bin/httpdate -v
/usr/local/bin/httpdate -h
```

## Systemd integration

### Create a dedicated user and group

Run the following on a system that uses `useradd` and `groupadd`:

```sh
sudo groupadd --system httpdate
sudo useradd --system \
  --gid httpdate \
  --no-create-home \
  --home-dir /nonexistent \
  --shell /usr/sbin/nologin \
  httpdate
```

If the `httpdate` group or user already exists, do not create it again. Check the existing account and adapt the commands to your distribution's account-management tools if necessary.

### Configure systemd

Create `/etc/systemd/system/httpdate.service`:

```ini
[Unit]
Description=HTTP Date Time Synchronization Daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/httpdate -F example.com https://example.com example.net
User=httpdate
Group=httpdate
CapabilityBoundingSet=CAP_SYS_TIME
AmbientCapabilities=CAP_SYS_TIME
Restart=on-failure
RestartSec=30

[Install]
WantedBy=multi-user.target
```

This configuration runs `httpdate` in the foreground under the dedicated `httpdate` account. The configured hosts and URLs are queried as supplied; if a server does not return a usable HTTP `Date` header, that sample is ignored.

`CAP_SYS_TIME` allows the service to adjust the system clock without running as root. Keep this capability limited to the service, as changing system time is a privileged operation.

Load the unit, enable it at boot, and start it:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now httpdate.service
```

## Check service status and logs

```sh
systemctl status httpdate.service
journalctl -u httpdate.service
journalctl -u httpdate.service -f
```

To restart or stop the service:

```sh
sudo systemctl restart httpdate.service
sudo systemctl stop httpdate.service
```

## Command-line options

| Option | Description |
| --- | --- |
| `-a` | Adjust time gradually using the system clock adjustment interface. |
| `-d` | Enable verbose/debug output; repeat for more detail. |
| `-D` | Run as a daemon in the background. |
| `-F` | Run daemon mode in the foreground, suitable for systemd. |
| `-h` | Display help. |
| `-p precision` | Set precision from 1 to 9; default is 3. |
| `-P proxy` | Use an HTTP proxy, for example `http://proxy:port`, `http://username:password@proxy:port`, or `host:port`. |
| `-s` | Set the system time immediately. |
| `-v` | Display version information. |
| `-x` | Adjust time and clock frequency to compensate for drift; intended for continuous daemon operation. |

Examples:

```sh
# Query servers and report the estimated offset without changing the clock
httpdate example.com

# Set the system clock immediately
sudo httpdate -s http://example.com

# Run continuously in the foreground with drift correction
httpdate -Fx -p8 https://example.com

# Run through a proxy server
httpdate -P 'http://username:password@proxy.example.net:8080' \
         https://www.example.com

# Alternatively, configure the proxy through environment variables
export https_proxy='http://username:password@proxy.example.net:8080'
httpdate https://www.example.com
```

The program also honors libcurl's standard proxy environment variables, including `http_proxy`, `https_proxy`, `HTTP_PROXY`, and `HTTPS_PROXY` where supported by libcurl.

If the username or password contains reserved URI characters such as `@`, `:`, `/`, or `#`, they should be URL-encoded.

## Troubleshooting

- **The service cannot adjust the clock:** confirm that the unit grants `CAP_SYS_TIME` and inspect `journalctl -u httpdate.service`.
- **No suitable servers are available:** check DNS, network access, proxy settings, and whether the configured servers return a valid HTTP `Date` header.
- **The time estimate is inaccurate:** Accuracy depends on server quality, network latency, and the precision setting. On networks where NTP is available, dedicated time-synchronization solutions such as `chrony`, `OpenNTPD`, `ntpd`, or `ntpd-rs` are generally preferable.
- **Proxy authentication fails:** verify the proxy URL format, ensure credentials are correct, and URL-encode reserved characters in usernames or passwords. You can also configure proxy settings through the standard `http_proxy` or `https_proxy` environment variables.
- **The service does not start:** check the executable path and permissions, then run `/usr/local/bin/httpdate -h` to verify the installed binary.

## License

`httpdate` is licensed under the GPL-2.0-or-later, the same license as its predecessor `htpdate`. See [LICENSE](LICENSE) for details.
