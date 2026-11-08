#!/bin/sh
# The helper's copies of the bridge's stem maths (helper/entry.c, between the "from the bridge"
# lines) must stay identical to bridge/rbstems_bridge.c's: entries the helper writes are only
# right if they are what the bridge would write.
set -eu
root="$(cd "$(dirname "$0")/../.." && pwd)"
fn() { sed -n "/^static [^(]* \**$2(/,/^}/p" "$1"; }
bad=0
for f in summarize residual all_finite monos corr_at rebuild_spec; do
  a="$(fn "$root/bridge/rbstems_bridge.c" $f)"
  b="$(fn "$root/helper/entry.c" $f)"
  if [ -z "$a" ] || [ "$a" != "$b" ]; then echo "helper/entry.c: $f differs from the bridge's" >&2; bad=1; fi
done
[ $bad = 0 ] && echo "helper: the bridge's stem maths, identical (6 functions)"
exit $bad
