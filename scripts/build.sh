#!/usr/bin/env bash
# Build deiner (E)SP32-App im Docker-Image (oder nativ) und sammle Artefakte in ./out
# Konfigurierbar per ENV:
# - DOCKER=1|0                 # Standard: 1 (im Container bauen)
# - DOCKER_IMAGE=…             # Standard: sle118/squeezelite-esp32-idfv435 (oder eigenes)
# - DOCKER_BIN=…               # z. B. "sudo docker" falls nötig; Standard: docker
# - IDF_TARGET=esp32           # esp32 / esp32s3 / …
# - VERSION=…                  # falls leer, wird aus Tag/Branch+Zeit+SHA erzeugt
# - EXTRA_IDF_ARGS=…           # z. B. "-DLOG_LEVEL=INFO"
# - OUT_DIR=out                # Artefaktausgabe
# - CCACHE_DIR=~/.ccache       # aktiviert ccache-Mount, falls vorhanden
# - BUILD_WEBAPP=1|0           # optionalen Webapp-Build steuern (Standard: 1)

set -Eeuo pipefail

DOCKER="${DOCKER:-1}"
DOCKER_IMAGE="${DOCKER_IMAGE:-sle118/squeezelite-esp32-idfv435}"
DOCKER_BIN="${DOCKER_BIN:-docker}"
IDF_TARGET="${IDF_TARGET:-esp32}"
OUT_DIR="${OUT_DIR:-out}"
EXTRA_IDF_ARGS="${EXTRA_IDF_ARGS:-}"
BUILD_WEBAPP="${BUILD_WEBAPP:-1}"
TZ="${TZ:-Europe/Berlin}"

# --- Hilfsfunktionen ---------------------------------------------------------

hash256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "Weder sha256sum noch shasum vorhanden" >&2
    exit 2
  fi
}

compute_version() {
  # 1) Git-Tag bevorzugen
  if git describe --tags --exact-match >/dev/null 2>&1; then
    git describe --tags --exact-match
    return
  fi
  # 2) Sonst Branch + Timestamp + ShortSHA
  local br ts sha
  br="$(git rev-parse --abbrev-ref HEAD | tr '/' '-')"
  ts="$(date +'%Y%m%d-%H%M')"
  sha="$(git rev-parse --short HEAD)"
  echo "${br}-${ts}-${sha}"
}

# --- Version ermitteln -------------------------------------------------------

VERSION="${VERSION:-}"
if [[ -z "$VERSION" ]]; then
  VERSION="$(compute_version)"
fi
echo "[build] VERSION=$VERSION"

# --- Clean Out-Verzeichnis ---------------------------------------------------

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

# --- Build-Logik -------------------------------------------------------------

build_inside() {
  # läuft im Build-Context (Host oder Container)
  set -Eeuo pipefail

  # --- Git Safe-Directory Workaround (Volume Ownership) ---
  # Wichtig: sowohl Workspace als auch Submodule erlauben
  git config --global --add safe.directory "$(pwd)" || true
  git config --global --add safe.directory "$(pwd)/components/esp-dsp" || true
  git config --global --add safe.directory "*" || true
  git config --global protocol.file.allow always || true

  echo "[build] Submodule sync/update…"
  git submodule sync --recursive
  if ! git submodule update --init --recursive; then
    echo "[build] Submodule-Update fehlgeschlagen, versuche esp-dsp Reset…"
    git submodule deinit -f components/esp-dsp || true
    rm -rf components/esp-dsp || true
    git submodule update --init --recursive --force
  fi

  # --- Optional: Web-UI bauen, falls vorhanden ---
  if [[ "${BUILD_WEBAPP}" != "0" && -d components/wifi-manager/webapp ]]; then
    if command -v npm >/dev/null 2>&1; then
      echo "[build] Build webapp…"
      pushd components/wifi-manager/webapp >/dev/null
      (npm ci || npm install)
      npm rebuild node-sass || true
      npm run build
      popd >/dev/null
    else
      echo "[build] npm nicht verfügbar – überspringe Webapp-Build."
    fi
  fi

  # --- ESP-IDF Build ---
  export IDF_TARGET="${IDF_TARGET}"
  echo "[build] idf.py build -DVERSION=${VERSION} ${EXTRA_IDF_ARGS:-}"
  idf.py build -DVERSION="${VERSION}" ${EXTRA_IDF_ARGS:-}

  # --- Artefakte sammeln ---
  echo "[build] Artefakte sammeln…"
  mkdir -p "${OUT_DIR}"

  # App-Binary ermitteln: alles außer Bootloader/Partition; nimm neuestes
  APP_BIN="$(ls -1t build/*.bin 2>/dev/null | grep -viE 'bootloader|partition' | head -n1 || true)"
  if [[ -z "${APP_BIN:-}" || ! -f "$APP_BIN" ]]; then
    echo "Konnte App-Binary nicht finden (build/*.bin). Prüfe Projektname/Build." >&2
    exit 3
  fi

  # Einheitlicher Name im OUT_DIR
  cp -v "$APP_BIN" "${OUT_DIR}/squeezelite.bin" || true

  # Optional mit ausliefern, falls vorhanden
  [[ -f build/recovery.bin ]] && cp -v build/recovery.bin "${OUT_DIR}/recovery.bin"
  [[ -f build/bootloader/bootloader.bin ]] && cp -v build/bootloader/bootloader.bin "${OUT_DIR}/bootloader.bin"
  [[ -f build/partition_table/partition-table.bin ]] && cp -v build/partition_table/partition-table.bin "${OUT_DIR}/partition-table.bin"

  # SHA256 & Metadata schreiben
  pushd "${OUT_DIR}" >/dev/null
  APP_SHA="$(hash256 squeezelite.bin)"
  echo "${APP_SHA}  squeezelite.bin" > sha256.txt

  cat > metadata.json <<EOF
{
  "version": "${VERSION}",
  "commit": "$(git rev-parse HEAD)",
  "built_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "idf_target": "${IDF_TARGET}",
  "extra_idf_args": "${EXTRA_IDF_ARGS}",
  "app_sha256": "${APP_SHA}"
}
EOF
  popd >/dev/null

  echo "[build] Fertig. Artefakte liegen in ${OUT_DIR}/"
}

if [[ "$DOCKER" == "1" ]]; then
  echo "[build] Baue im Docker-Image: ${DOCKER_IMAGE}"
  # Optional ccache mount
  CCACHE_DIR_MOUNT=""
  if [[ -n "${CCACHE_DIR:-}" && -d "${CCACHE_DIR}" ]]; then
    CCACHE_DIR_MOUNT="-v ${CCACHE_DIR}:/root/.ccache -e IDF_CCACHE_ENABLE=1"
  fi

  ${DOCKER_BIN} pull "${DOCKER_IMAGE}" >/dev/null 2>&1 || true
  ${DOCKER_BIN} run --rm \
    -e TZ="${TZ}" \
    -e VERSION="${VERSION}" \
    -e IDF_TARGET="${IDF_TARGET}" \
    -e EXTRA_IDF_ARGS="${EXTRA_IDF_ARGS}" \
    -e BUILD_WEBAPP="${BUILD_WEBAPP}" \
    -v "$PWD":/workspace \
    ${CCACHE_DIR_MOUNT:-} \
    -w /workspace \
    "${DOCKER_IMAGE}" \
    bash -lc "$(declare -f hash256); $(declare -f build_inside); build_inside"
else
  echo "[build] Baue nativ auf dem Host (idf.py muss verfügbar sein)"
  build_inside
fi

echo "[build] Done."
