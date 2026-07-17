#!/usr/bin/env bash
set -euo pipefail

user_home="/mnt/c/Users/${GRIMVAULT_WINDOWS_USER:-Ethan}/AppData"
if [[ $# -gt 0 ]]; then
  search_dirs=("$1")
else
  search_dirs=(
    "$user_home/Local/GrimVault/logs"
    "$user_home/Roaming/GrimVault/logs"
    "$user_home/Roaming/DDB/GrimVault/logs"
  )
fi
log_file="$(find "${search_dirs[@]}" -maxdepth 1 -type f \
  \( -name '*.log' -o -name 'grimvault*.txt' \) -printf '%T@ %p\n' 2>/dev/null \
  | sort -nr | head -1 | cut -d' ' -f2-)"
if [[ -z "$log_file" ]]; then
  echo "No GrimVault log file found" >&2
  exit 1
fi
log_dir="$(dirname "$log_file")"

echo "Log: $log_file"
echo
echo "Event counts:"
rg -o '\[vision\] (anchor_[a-z_]+|replacement_[a-z_]+)' "$log_file" \
  | sed 's/.*\[vision\] //' | sort | uniq -c | sort -nr || true
echo
echo "Latest anchoring events:"
rg '\[vision\] (anchor_|replacement_)' "$log_file" | tail -120 || true
echo
echo "Diagnostic frames: $log_dir/anchoring"
