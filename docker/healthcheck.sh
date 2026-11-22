#!/bin/bash
# Docker HEALTHCHECK probe: GET /health on the server inside this container.
#
# Uses bash's /dev/tcp so that the runtime image needs no curl or wget.
# /health is a liveness check (HTTP loop answers); it does not test storage,
# codecs or a Lua policy backend.
#
# The port follows SIPI_PORT (default 8080), the same variable the server reads.
# If the port is set only by --port or a TOML file, also set SIPI_PORT or
# override the HEALTHCHECK. The probe connects to 127.0.0.1, which works with
# the image's default listen address 0.0.0.0.
set -u
port="${SIPI_PORT:-8080}"
case "$port" in
    ''|*[!0-9]*) echo "healthcheck: invalid SIPI_PORT" >&2; exit 1 ;;
esac
exec 3<>"/dev/tcp/127.0.0.1/${port}" || exit 1
printf 'GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' >&3
IFS= read -r -t 5 status <&3 || exit 1
case "$status" in
    "HTTP/1."[01]" 200 "*) exit 0 ;;
    *) echo "healthcheck: unexpected response: ${status%$'\r'}" >&2; exit 1 ;;
esac
