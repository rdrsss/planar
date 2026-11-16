#!/bin/sh
set -eu
# Verify identities before relying on the local build; this does not rebuild it.
base=$(docker image inspect planar-t044-agent-preflight:latest --format '{{.Id}}')
[ "$base" = 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7' ]
image='sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2'
[ "$(docker image inspect "$image" --format '{{.Id}}')" = "$image" ]
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT HUP INT TERM
docker run --rm --network none --entrypoint /bin/sh "$image" -c 'dpkg-query -W -f="\${Package}=\${Version}\n" | LC_ALL=C sort' > "$tmp"
cmp "$(dirname "$0")/installed-packages.txt" "$tmp"
