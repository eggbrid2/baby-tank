#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_root="${IDF_PATH:-$repo_root/.tools/esp-idf-v6.1}"
generator="$idf_root/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
output="${1:-$repo_root/firmware/head/build/credentials-nvs.bin}"

: "${BABY_TANK_DEVICE_TOKEN:?set BABY_TANK_DEVICE_TOKEN in the environment}"
: "${BABY_TANK_SETUP_POP:?set BABY_TANK_SETUP_POP in the environment}"
wifi_ssid="${BABY_TANK_WIFI_SSID-}"
wifi_password="${BABY_TANK_WIFI_PASSWORD-}"

if [[ ${#wifi_ssid} -gt 32 ]]; then
  echo "Wi-Fi SSID exceeds 32 bytes" >&2
  exit 2
fi
if [[ ${#wifi_password} -gt 64 ]]; then
  echo "Wi-Fi password exceeds 64 bytes" >&2
  exit 2
fi
if [[ ${#BABY_TANK_DEVICE_TOKEN} -lt 24 || ${#BABY_TANK_DEVICE_TOKEN} -gt 191 ]]; then
  echo "Device token length must be 24..191 bytes" >&2
  exit 2
fi
if [[ ${#BABY_TANK_SETUP_POP} -lt 8 || ${#BABY_TANK_SETUP_POP} -gt 64 ]]; then
  echo "Setup code length must be 8..64 bytes" >&2
  exit 2
fi
if [[ ! -f "$generator" ]]; then
  echo "ESP-IDF NVS generator not found: $generator" >&2
  exit 2
fi

# The IDF 6.1 generator is a Python module installed in ESP-IDF's managed
# virtual environment, not in the host Python installation.
if ! python -c 'import esp_idf_nvs_partition_gen' >/dev/null 2>&1; then
  # shellcheck disable=SC1091
  source "$idf_root/export.sh" >/dev/null 2>&1
fi

csv_file="$(mktemp -t baby-tank-nvs.XXXXXX.csv)"
trap 'rm -f "$csv_file"' EXIT

csv_quote() {
  local value=${1//\"/\"\"}
  printf '"%s"' "$value"
}

{
  printf 'key,type,encoding,value\n'
  printf 'baby_tank,namespace,,\n'
  printf 'wifi_ssid,data,string,'; csv_quote "$wifi_ssid"; printf '\n'
  printf 'wifi_pass,data,string,'; csv_quote "$wifi_password"; printf '\n'
  printf 'device_token,data,string,'; csv_quote "$BABY_TANK_DEVICE_TOKEN"; printf '\n'
  printf 'setup_pop,data,string,'; csv_quote "$BABY_TANK_SETUP_POP"; printf '\n'
} >"$csv_file"

mkdir -p "$(dirname "$output")"
python "$generator" generate "$csv_file" "$output" 0x4000
chmod 600 "$output"
echo "Generated NVS image: $output"
echo "No hardware was flashed. Treat this file as a secret."
