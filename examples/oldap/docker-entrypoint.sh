#!/bin/sh
# Translate the existing OLDAP deployment's environment at the container boundary.
# Never trace this script: it handles credentials. Explicit SIPI settings win.
set -eu
if [ -z "${SIPI_PREFLIGHT_SECRETS_MEDIA_JWT_KEY+x}" ] && [ -z "${SIPI_PREFLIGHT_SECRET_FILES_MEDIA_JWT_KEY+x}" ]; then
    : "${OLDAP_MEDIA_JWT_SECRET:?OLDAP media key is required}"
    export SIPI_PREFLIGHT_SECRETS_MEDIA_JWT_KEY="$OLDAP_MEDIA_JWT_SECRET"
fi
export SIPI_PREFLIGHT_CONFIG_JWT_ISSUER="${SIPI_PREFLIGHT_CONFIG_JWT_ISSUER-${OLDAP_JWT_ISSUER-https://oldap.org}}"
export SIPI_PREFLIGHT_CONFIG_JWT_AUDIENCE="${SIPI_PREFLIGHT_CONFIG_JWT_AUDIENCE-${OLDAP_MEDIA_JWT_AUDIENCE-oldap-api-media}}"
# Preserve the delegate's existing image permission threshold explicitly.
export SIPI_PREFLIGHT_CONFIG_MINIMUM_PERMISSION="${SIPI_PREFLIGHT_CONFIG_MINIMUM_PERMISSION-1}"
# Anonymous lookup is an explicit opt-in; do not guess missing account credentials.
case "${OLDAP_SIPI_ANONYMOUS-false}" in
    true)
        : "${OLDAP_API_URL:?Anonymous lookup requires OLDAP_API_URL}"
        export SIPI_PREFLIGHT_CONFIG_API_URL="${SIPI_PREFLIGHT_CONFIG_API_URL-$OLDAP_API_URL}"
        export SIPI_PREFLIGHT_CONFIG_UNKNOWN_USER="${SIPI_PREFLIGHT_CONFIG_UNKNOWN_USER-${OLDAP_UNKNOWN_USERID-unknown}}"
        if [ -z "${SIPI_PREFLIGHT_SECRETS_UNKNOWN_PASSWORD+x}" ] && [ -z "${SIPI_PREFLIGHT_SECRET_FILES_UNKNOWN_PASSWORD+x}" ] && [ -n "${OLDAP_UNKNOWN_PASSWORD-}" ]; then
            export SIPI_PREFLIGHT_SECRETS_UNKNOWN_PASSWORD="$OLDAP_UNKNOWN_PASSWORD"
        fi
        ;;
    false) ;;
    *) echo 'OLDAP_SIPI_ANONYMOUS must be true or false' >&2; exit 2 ;;
esac
exec /opt/sipi/sipi-2 "$@"
