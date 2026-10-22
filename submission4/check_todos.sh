#!/bin/sh

set -eu

cd "$(dirname "$0")"

show_locations=1
if [ "${1:-}" = "--count" ] || [ "${1:-}" = "-c" ]; then
  show_locations=0
fi

# Find all occurrences of not_implemented in starter source files (excluding framework and declaration header)
source_files=$(find . -maxdepth 2 -type f \( -name '*.cc' -o -name '*.h' \) \
  ! -path './include/pa4.h' \
  ! -path './framework/*' \
  ! -path './.build/*' | sort)

matches=$(echo "$source_files" | xargs grep -HnE 'not_implemented\("PA4-[A-Z0-9-]+"\)' 2>/dev/null || true)

if [ -z "$matches" ]; then
  marker_count=0
else
  marker_count=$(echo "$matches" | grep -c 'not_implemented' || true)
fi

echo "=========================================="
echo "        CS6600 PA4 TODO Status            "
echo "=========================================="
echo "Remaining 'not_implemented' markers: $marker_count"
echo ""

if [ "$marker_count" -gt 0 ]; then
  echo "--- Breakdown by Student Ownership ---"
  s1_count=$(echo "$matches" | grep -c -E 'PA4-COH-0[34]' || true)
  s2_count=$(echo "$matches" | grep -c -E 'PA4-COH-0[12]|PA4-ORDER-01' || true)
  s3_count=$(echo "$matches" | grep -c -E 'PA4-NOC-0[12]|PA4-MC-01|PA4-COUNT-01' || true)

  echo "  Student 1 (Directory & Coherence) : $s1_count remaining"
  echo "  Student 2 (Cache & Ordering)      : $s2_count remaining"
  echo "  Student 3 (NoC, DRAM, Counters)   : $s3_count remaining"
  echo ""

  if [ "$show_locations" -eq 1 ]; then
    echo "--- Locations & Tasks ---"
    while IFS= read -r line; do
      [ -z "$line" ] && continue
      file=$(echo "$line" | cut -d: -f1)
      lineno=$(echo "$line" | cut -d: -f2)
      tag=$(echo "$line" | grep -oE 'PA4-[A-Z0-9-]+')
      
      case "$tag" in
        PA4-COH-01) owner="Student 2 | accept_cache_request" ;;
        PA4-COH-02) owner="Student 2 | handle_cache_message" ;;
        PA4-COH-03) owner="Student 1 | handle_directory_message" ;;
        PA4-COH-04) owner="Student 1 | advance_directory_transaction" ;;
        PA4-ORDER-01) owner="Student 2 | decide_memory_action" ;;
        PA4-MC-01) owner="Student 3 | select_memory_request" ;;
        PA4-NOC-01) owner="Student 3 | select_output_port" ;;
        PA4-NOC-02) owner="Student 3 | select_router_input" ;;
        PA4-COUNT-01) owner="Student 3 | count_event" ;;
        *) owner="Unassigned" ;;
      esac
      
      printf "  %-14s %-26s : line %-4s [%s]\n" "$tag" "$file" "$lineno" "$owner"
    done <<EOF
$matches
EOF
    echo ""
    echo "Note: PA4-COUNT-02 (sample_occupancy_and_stalls in statistics.cc) is also pending implementation."
  fi
  exit 1
else
  echo "All 'not_implemented' markers have been resolved!"
  exit 0
fi
