#!/bin/sh
# hubblob.sh REPO DIGEST OUT: fetches one blob (an image layer) of a Docker Hub
# repository to OUT and checks it against its sha256 digest. The registry
# answers 502 now and then, and a token it gives out lasts a few minutes, so
# each of up to 5 tries takes a fresh token, with a growing pause between.
set -u
repo=$1 digest=$2 out=$3
n=0
while [ $n -lt 5 ]; do
  if [ $n -gt 0 ]; then
    echo "hubblob: $repo ${digest#sha256:}: try $((n + 1)) after a pause" >&2
    sleep $((n * 5))
  fi
  n=$((n + 1))
  tok=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$repo:pull" | jq -r .token) || continue
  curl -fsSL -H "Authorization: Bearer $tok" -o "$out.part" "https://registry-1.docker.io/v2/$repo/blobs/$digest" || continue
  if echo "${digest#sha256:}  $out.part" | sha256sum -c - >/dev/null; then
    mv "$out.part" "$out"
    exit 0
  fi
done
rm -f "$out.part"
echo "hubblob: $repo $digest: no good copy after $n tries" >&2
exit 1
