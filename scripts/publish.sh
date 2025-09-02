#!/usr/bin/env bash
# Veröffentlicht Artefakte aus ./out in ein OTA-Verzeichnis und aktualisiert index.json + 'latest' Symlink.
# Konfigurierbar per ENV:
# - OTA_ROOT=/var/www/ota/squeezelite   # Zielbasis (Webroot)
# - VERSION=…                           # falls leer, aus ./out/metadata.json gelesen
# - USE_SUDO=1|0                        # Standard: 0 (bei Bedarf 1 setzen)
# - PROJECT_NAME=squeezelite            # für index.json
#
# Aufruf: scripts/publish.sh
# Erwartet: ./out/squeezelite.bin (+ sha256.txt, metadata.json)

set -Eeuo pipefail

OTA_ROOT="${OTA_ROOT:-/var/www/ota/squeezelite}"
PROJECT_NAME="${PROJECT_NAME:-squeezelite}"
USE_SUDO="${USE_SUDO:-0}"

_sudo() {
  if [[ "${USE_SUDO}" == "1" ]]; then sudo "$@"; else "$@"; fi
}

require() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Fehlt: $1" >&2
    exit 2
  }
}

# --- Artefakte prüfen --------------------------------------------------------

if [[ ! -f out/squeezelite.bin ]]; then
  echo "Erwarte out/squeezelite.bin — bitte vorher build.sh ausführen." >&2
  exit 3
fi
if [[ ! -f out/metadata.json ]]; then
  echo "Erwarte out/metadata.json — wurde im Build erzeugt?" >&2
  exit 3
fi

# --- VERSION bestimmen -------------------------------------------------------

if [[ -z "${VERSION:-}" ]]; then
  if command -v jq >/dev/null 2>&1; then
    VERSION="$(jq -r '.version' out/metadata.json)"
  else
    VERSION="$(grep -oE '"version"\s*:\s*"[^"]+"' out/metadata.json | head -n1 | sed 's/.*:"\([^"]*\)".*/\1/')"
  fi
fi

if [[ -z "${VERSION:-}" ]]; then
  echo "Konnte VERSION nicht ermitteln." >&2
  exit 4
fi

DEST="${OTA_ROOT}/${VERSION}"
DATE="$(date +%F)"

echo "[publish] OTA_ROOT=${OTA_ROOT}"
echo "[publish] VERSION=${VERSION}"
echo "[publish] DEST=${DEST}"

# --- Kopieren ----------------------------------------------------------------

_sudo mkdir -p "${DEST}"
_sudo cp -v out/* "${DEST}/"

# Rechte/Owner optional anpassen (hier nur lesbar stellen)
_sudo chmod -R a+r "${DEST}"
_sudo find "${OTA_ROOT}" -type d -exec chmod a+rx {} \; >/dev/null 2>&1 || true

# Latest-Symlink aktualisieren
pushd "${OTA_ROOT}" >/dev/null
_sudo ln -sfn "${VERSION}" latest
popd >/dev/null

# --- index.json aktualisieren ------------------------------------------------
INDEX="${OTA_ROOT}/index.json"
TMP="$(mktemp)"

if command -v jq >/dev/null 2>&1; then
  if [[ -f "${INDEX}" ]]; then
    # Bestehende Builds übernehmen, Version ersetzen/anhängen, latest setzen
    jq --arg v "$VERSION" --arg d "$DATE" --arg p "$PROJECT_NAME" '
      .project = $p
      | .latest = $v
      | .builds = (
          ( (.builds // []) | map(select(.version != $v)) )
          + [ { "version": $v, "date": $d, "url": ("./"+$v+"/squeezelite.bin") } ]
        )
    ' "${INDEX}" > "${TMP}"
  else
    jq -n --arg v "$VERSION" --arg d "$DATE" --arg p "$PROJECT_NAME" '
      { project: $p, latest: $v,
        builds: [ {version: $v, date: $d, url: ("./"+$v+"/squeezelite.bin")} ] }' > "${TMP}"
  fi
  _sudo mv "${TMP}" "${INDEX}"
else
  # Minimaler Fallback ohne jq
  cat > "${TMP}" <<JSON
{"project":"${PROJECT_NAME}","latest":"${VERSION}","builds":[{"version":"${VERSION}","date":"${DATE}","url":"./${VERSION}/squeezelite.bin"}]}
JSON
  _sudo mv "${TMP}" "${INDEX}"
fi

_sudo chmod a+r "${INDEX}"

echo "[publish] Fertig. Bereit unter: ${OTA_ROOT}/${VERSION}"
echo "[publish] Latest-Link: ${OTA_ROOT}/latest  → ${VERSION}"
