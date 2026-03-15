#!/bin/bash
set -euo pipefail

BASE_URL="${IRCORD_DOWNLOAD_BASE_URL:-https://chat.rausku.com/downloads}"
MANIFEST_URL="${IRCORD_MANIFEST_URL:-${BASE_URL}/installer-manifest.json}"
TMP_DIR="$(mktemp -d)"
INSTALLER_PATH="${TMP_DIR}/ircord-installer"

cleanup() {
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

detect_platform() {
    local os arch
    os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    arch="$(uname -m)"

    if [ "${os}" != "linux" ]; then
        echo "Unsupported OS: ${os}" >&2
        exit 1
    fi

    case "${arch}" in
        x86_64|amd64) echo "linux-x64" ;;
        aarch64|arm64) echo "linux-arm64" ;;
        *)
            echo "Unsupported architecture: ${arch}" >&2
            exit 1
            ;;
    esac
}

PLATFORM="$(detect_platform)"
INSTALLER_URL="${IRCORD_INSTALLER_URL:-${BASE_URL}/ircord-installer-${PLATFORM}}"

echo "IRCord installer bootstrap"
echo "Platform: ${PLATFORM}"
echo "Installer: ${INSTALLER_URL}"

curl -fL --silent --show-error --output "${INSTALLER_PATH}" "${INSTALLER_URL}"
chmod +x "${INSTALLER_PATH}"

if [ "$(id -u)" -eq 0 ]; then
    exec "${INSTALLER_PATH}" --manifest-url "${MANIFEST_URL}"
fi

if command -v sudo >/dev/null 2>&1; then
    exec sudo "${INSTALLER_PATH}" --manifest-url "${MANIFEST_URL}"
fi

echo "Root privileges are required. Re-run with sudo:" >&2
echo "  curl -fsSL ${BASE_URL}/install.sh | sudo bash" >&2
exit 1
