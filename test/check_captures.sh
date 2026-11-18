#!/bin/sh
# Decode each captured Canon job, build it again with pbm2ufr and compare.
# If the encoder is right the files come out byte for byte the same.
set -e
B=${1:-build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
for cap in test/captures/*.ufr; do
  n=$(basename "$cap" .ufr)
  python3 tools/ufrdump.py bands "$cap" "$T/$n" > /dev/null
  jbgtopbm "$T/$n/page1.jbg" "$T/$n.pbm"
  eval "$(python3 tools/ufrdump.py jobinfo "$cap")"
  "$B/pbm2ufr" --name "$NAME" --user "$USER_NAME" --host "$HOST" --job "$JOB" --time "$TIME" \
    "$T/$n.pbm" "$T/$n.out" 2> /dev/null
  if cmp -s "$cap" "$T/$n.out"; then
    echo "$n: identical to the Canon driver output"
  else
    echo "$n: DIFFERENT"
    fail=1
  fi
done
exit $fail
