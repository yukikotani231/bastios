#!/bin/sh
# Pulls an image from an OCI (or Docker) registry and turns it into a bundle
# that runc or crun runs:
#
#     ocipull.sh [-r] [-a ARCH] IMAGE BUNDLE
#
#     ocipull.sh alpine:3.24 /run/b           # docker.io/library/alpine:3.24
#     ocipull.sh -r ghcr.io/org/app@sha256:... /home/me/b
#
# IMAGE is [REGISTRY/]NAME[:TAG | @DIGEST]; a name with no registry is on
# Docker Hub (and one with no "/" is under library/). BUNDLE/rootfs gets the
# layers in order, with their whiteouts applied, each checked against its
# digest; BUNDLE/config.json is what `runc spec` makes, with the image's
# command, environment and working directory in it. -r makes the
# spec for a rootless container (runc spec --rootless), which the user who
# runs it owns; ARCH is the platform to take from an index (the machine's
# by default). A registry that wants a token gets one from the realm it
# names (anonymous, which is what a public image needs).
#
# Needs curl, jq, tar, gzip and sha256sum (BusyBox's do), and runc (RUNC
# names it if it is not on the PATH). OCI_HTTP=1 talks plain HTTP to the
# registry, for one on a local network.
set -eu

rootless=
arch=
while [ $# -gt 0 ]; do
    case $1 in
    -r) rootless=--rootless; shift ;;
    -a) arch=$2; shift 2 ;;
    --) shift; break ;;
    -*) echo "ocipull: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
done
if [ $# -ne 2 ]; then
    echo "usage: ocipull.sh [-r] [-a ARCH] IMAGE BUNDLE" >&2
    exit 2
fi
image=$1
bundle=$2
runc=${RUNC:-runc}

if [ -z "$arch" ]; then
    case $(uname -m) in
    x86_64) arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) arch=$(uname -m) ;;
    esac
fi

# Registry, repository and reference.
ref=latest
name=$image
case $name in
*@*) ref=${name#*@}; name=${name%%@*} ;;
*)
    last=${name##*/}
    case $last in
    *:*) ref=${last#*:}; name=${name%:*} ;;
    esac
    ;;
esac
registry=registry-1.docker.io
case $name in
*/*)
    first=${name%%/*}
    case $first in
    *.*|*:*|localhost) registry=$first; name=${name#*/} ;;
    esac
    ;;
esac
case $registry:$name in
registry-1.docker.io:*/*) ;;
registry-1.docker.io:*) name=library/$name ;;
esac
scheme=https
[ -n "${OCI_HTTP:-}" ] && scheme=http
base=$scheme://$registry/v2/$name

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
token=

# fetch URL FILE [ACCEPT]: the body of URL in FILE, after the token the
# registry asks for if it does (once).
fetch() {
    tries=0
    while :; do
        if [ -n "$token" ]; then
            code=$(curl -sS -L -o "$2" -D "$work/hdr" -w '%{http_code}' \
                -H "Authorization: Bearer $token" ${3:+-H "Accept: $3"} "$1")
        else
            code=$(curl -sS -L -o "$2" -D "$work/hdr" -w '%{http_code}' ${3:+-H "Accept: $3"} "$1")
        fi
        case $code in
        200) return 0 ;;
        401)
            tries=$((tries + 1))
            [ $tries -gt 1 ] && { echo "ocipull: $1: not authorised" >&2; return 1; }
            get_token || return 1
            ;;
        *) echo "ocipull: $1: HTTP $code" >&2; return 1 ;;
        esac
    done
}

# get_token: from the Bearer challenge in the last response.
get_token() {
    challenge=$(tr -d '\r' < "$work/hdr" | grep -i '^www-authenticate: *bearer' | head -n 1) || true
    [ -n "$challenge" ] || { echo "ocipull: the registry wants a login, which is not supported" >&2; return 1; }
    realm=$(printf '%s\n' "$challenge" | sed -n 's/.*realm="\([^"]*\)".*/\1/p')
    service=$(printf '%s\n' "$challenge" | sed -n 's/.*service="\([^"]*\)".*/\1/p')
    q="service=$service&scope=repository:$name:pull"
    curl -sS -L -o "$work/token" "$realm?$q" || return 1
    token=$(jq -r '.token // .access_token' "$work/token")
    [ -n "$token" ] && [ "$token" != null ]
}

# digest_ok FILE DIGEST
digest_ok() {
    [ "sha256:$(sha256sum "$1" | cut -d ' ' -f 1)" = "$2" ]
}

accept='application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json'

fetch "$base/manifests/$ref" "$work/manifest" "$accept"
# An index: the manifest for this platform.
if jq -e '.manifests' "$work/manifest" > /dev/null; then
    d=$(jq -r --arg a "$arch" '[.manifests[] | select(.platform.os == "linux" and .platform.architecture == $a)][0].digest // empty' "$work/manifest")
    [ -n "$d" ] || { echo "ocipull: $image has no linux/$arch image" >&2; exit 1; }
    fetch "$base/manifests/$d" "$work/manifest" "$accept"
    digest_ok "$work/manifest" "$d" || { echo "ocipull: the manifest does not match $d" >&2; exit 1; }
elif [ "${ref#sha256:}" != "$ref" ]; then
    digest_ok "$work/manifest" "$ref" || { echo "ocipull: the manifest does not match $ref" >&2; exit 1; }
fi

cfg=$(jq -r '.config.digest' "$work/manifest")
fetch "$base/blobs/$cfg" "$work/config"
digest_ok "$work/config" "$cfg" || { echo "ocipull: the image config does not match $cfg" >&2; exit 1; }

mkdir -p "$bundle/rootfs"
rootfs=$(cd "$bundle/rootfs" && pwd)
for layer in $(jq -r '.layers[].digest' "$work/manifest"); do
    fetch "$base/blobs/$layer" "$work/layer"
    digest_ok "$work/layer" "$layer" || { echo "ocipull: layer $layer does not match its digest" >&2; exit 1; }
    # Whiteouts hide what the layers below made: .wh.NAME removes NAME, and
    # .wh..wh..opq empties its directory; neither is extracted.
    tar -tzf "$work/layer" | while IFS= read -r f; do
        b=${f##*/}
        d=${f%"$b"}
        case $b in
        .wh..wh..opq) find "$rootfs/$d" -mindepth 1 -maxdepth 1 -exec rm -rf {} \; ;;
        .wh.*) rm -rf "${rootfs:?}/$d${b#.wh.}" ;;
        esac
    done
    tar -xzf "$work/layer" -C "$rootfs" --exclude '.wh.*'
done

# config.json: the runtime's default spec, with the image's process in it.
(cd "$bundle" && rm -f config.json && "$runc" spec $rootless)
jq --slurpfile img "$work/config" '
    ($img[0].config // {}) as $c
    | .process.args = (($c.Entrypoint // []) + ($c.Cmd // []))
    | .process.args |= (if length == 0 then ["/bin/sh"] else . end)
    | .process.env = ((($c.Env // []) + (.process.env // [])) | unique_by(split("=")[0]))
    | .process.cwd = (if ($c.WorkingDir // "") == "" then "/" else $c.WorkingDir end)
    | .process.terminal = false
    | .root.path = "rootfs"
' "$bundle/config.json" > "$work/spec"
mv "$work/spec" "$bundle/config.json"
echo "$image: $(jq -r '.layers | length' "$work/manifest") layers in $bundle"
