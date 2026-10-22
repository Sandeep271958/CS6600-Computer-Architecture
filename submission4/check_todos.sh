#!/bin/sh

set -eu
cd "$(dirname "$0")"

# Check terminal color capability
if [ -t 1 ]; then
  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  BLUE='\033[0;34m'
  BOLD='\033[1m'
  NC='\033[0m'
else
  GREEN=''
  RED=''
  YELLOW=''
  BLUE=''
  BOLD=''
  NC=''
fi

# Master definition of all 10 PA4 TODO tasks:
# Format: TAG|FILE|OWNER|FUNCTION_NAME
TASKS="PA4-COH-03|coherence/directory.cc|Student 1|handle_directory_message
PA4-COH-04|coherence/directory.cc|Student 1|advance_directory_transaction
PA4-COH-01|coherence/cache.cc|Student 2|accept_cache_request
PA4-COH-02|coherence/cache.cc|Student 2|handle_cache_message
PA4-ORDER-01|memory/ordering.cc|Student 2|decide_memory_action
PA4-NOC-01|noc/routing.cc|Student 3|select_output_port
PA4-NOC-02|noc/routing.cc|Student 3|select_router_input
PA4-MC-01|memory/scheduling.cc|Student 3|select_memory_request
PA4-COUNT-01|statistics.cc|Student 3|count_event
PA4-COUNT-02|statistics.cc|Student 3|sample_occupancy_and_stalls"

total_count=0
done_count=0
pending_count=0

s1_total=0; s1_done=0
s2_total=0; s2_done=0
s3_total=0; s3_done=0

done_output=""
pending_output=""

NL='
'
OLD_IFS="$IFS"
IFS="$NL"

for item in $TASKS; do
  [ -z "$item" ] && continue

  tag=$(echo "$item" | cut -d'|' -f1)
  file=$(echo "$item" | cut -d'|' -f2)
  owner=$(echo "$item" | cut -d'|' -f3)
  func=$(echo "$item" | cut -d'|' -f4)

  total_count=$((total_count + 1))
  case "$owner" in
    "Student 1") s1_total=$((s1_total + 1)) ;;
    "Student 2") s2_total=$((s2_total + 1)) ;;
    "Student 3") s3_total=$((s3_total + 1)) ;;
  esac

  is_pending=0
  loc_str="$file"

  if [ "$tag" = "PA4-COUNT-02" ]; then
    if grep -q '/\* system \*/' "$file" 2>/dev/null || grep -q 'not_implemented("PA4-COUNT-02")' "$file" 2>/dev/null; then
      is_pending=1
      lineno=$(grep -n 'sample_occupancy_and_stalls' "$file" 2>/dev/null | cut -d: -f1 | head -n1 || echo "")
      [ -n "$lineno" ] && loc_str="$file:$lineno"
    fi
  else
    hit=$(grep -Hn "not_implemented(\"$tag\")" "$file" 2>/dev/null || true)
    if [ -n "$hit" ]; then
      is_pending=1
      lineno=$(echo "$hit" | cut -d: -f2 | head -n1)
      loc_str="$file:$lineno"
    fi
  fi

  if [ "$is_pending" -eq 1 ]; then
    pending_count=$((pending_count + 1))
    pending_output="${pending_output}  [${RED}TODO ✗${NC}]  $(printf '%-13s' "$tag") : $(printf '%-26s' "$loc_str") [${owner} | ${func}]${NL}"
  else
    done_count=$((done_count + 1))
    case "$owner" in
      "Student 1") s1_done=$((s1_done + 1)) ;;
      "Student 2") s2_done=$((s2_done + 1)) ;;
      "Student 3") s3_done=$((s3_done + 1)) ;;
    esac
    done_output="${done_output}  [${GREEN}DONE ✓${NC}]  $(printf '%-13s' "$tag") : $(printf '%-26s' "$file") [${owner} | ${func}]${NL}"
  fi
done

IFS="$OLD_IFS"

pct=$((done_count * 100 / total_count))

echo "================================================================"
echo "                   CS6600 PA4 TODO Progress                     "
echo "================================================================"
printf "Total Tasks: %d | Completed: ${GREEN}%d${NC} | Remaining: ${RED}%d${NC} (${BOLD}%d%%${NC} complete)\n\n" \
  "$total_count" "$done_count" "$pending_count" "$pct"

echo "--- Progress by Student Ownership ---"
printf "  Student 1 (Directory & LLC)   : %d / %d completed (%2d%%)\n" \
  "$s1_done" "$s1_total" "$((s1_done * 100 / s1_total))"
printf "  Student 2 (Cache & Ordering)  : %d / %d completed (%2d%%)\n" \
  "$s2_done" "$s2_total" "$((s2_done * 100 / s2_total))"
printf "  Student 3 (NoC, DRAM, Counts) : %d / %d completed (%2d%%)\n\n" \
  "$s3_done" "$s3_total" "$((s3_done * 100 / s3_total))"

if [ "$done_count" -gt 0 ]; then
  echo "--- Completed Tasks ($done_count) ---"
  printf "%b" "$done_output"
  echo ""
fi

if [ "$pending_count" -gt 0 ]; then
  echo "--- Remaining Tasks ($pending_count) ---"
  printf "%b" "$pending_output"
  echo ""
  exit 1
else
  echo "${GREEN}All $total_count PA4 tasks have been completed successfully!${NC}"
  exit 0
fi
