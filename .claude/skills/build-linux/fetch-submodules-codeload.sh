#!/usr/bin/env bash
#
# Populate the submodules needed for a Linux build via codeload.github.com tarballs.
# Use this when `git clone` of submodules is blocked (agent sandbox proxy returns 403),
# but codeload / api.github.com are reachable. SHAs are read live from the superproject
# tree, so this stays correct as submodule pins change.
#
# Usage:  bash .claude/skills/build-linux/fetch-submodules-codeload.sh
set -uo pipefail
cd "$(git rev-parse --show-toplevel)"

# Submodules required for a Linux (no-CUDA-headers-needed beyond nv-codec) build.
NEEDED=(
  third-party/moonlight-common-c
  third-party/Simple-Web-Server
  third-party/nanors
  third-party/tray
  third-party/inputtino
  third-party/libdisplaydevice
  third-party/glad
  third-party/nv-codec-headers
  third-party/wayland-protocols
  third-party/wlr-protocols
  third-party/plasma-wayland-protocols
  third-party/libvirtualdisplay
  third-party/build-deps
)

# owner/repo from a github(.git) URL
repo_of() { sed -E 's#^https?://github.com/##; s/\.git$//' <<<"$1"; }

fetch_tarball() { # owner/repo  sha  destdir
  local repo="$1" sha="$2" dest="$3"
  [ -f "$dest/.populated" ] && { echo "skip  $dest"; return 0; }
  mkdir -p "$dest"
  echo ">> $repo @ ${sha:0:10} -> $dest"
  if curl -fsSL --retry 3 "https://codeload.github.com/$repo/tar.gz/$sha" \
       | tar xz -C "$dest" --strip-components=1; then
    touch "$dest/.populated"
  else
    echo "!! FAILED $repo $sha" >&2; return 1
  fi
}

for path in "${NEEDED[@]}"; do
  name=$(git config -f .gitmodules --get-regexp '\.path$' | awk -v p="$path" '$2==p{print $1}' | sed -E 's/^submodule\.//; s/\.path$//')
  url=$(git config -f .gitmodules --get "submodule.${name}.url")
  sha=$(git ls-tree HEAD "$path" | awk '{print $3}')
  [ -n "$url" ] && [ -n "$sha" ] || { echo "!! no url/sha for $path" >&2; continue; }
  fetch_tarball "$(repo_of "$url")" "$sha" "$path"
done

# Nested submodules: resolve the pinned SHA via the contents API, then codeload it.
# moonlight-common-c needs enet at its pinned commit (it is in the include path).
mcc_sha=$(git ls-tree HEAD third-party/moonlight-common-c | awk '{print $3}')
if [ -n "$mcc_sha" ] && [ ! -f third-party/moonlight-common-c/enet/.populated ]; then
  enet_sha=$(curl -fsSL "https://api.github.com/repos/moonlight-stream/moonlight-common-c/contents/enet?ref=$mcc_sha" \
              | grep -m1 '"sha"' | sed -E 's/.*"sha": *"([0-9a-f]+)".*/\1/')
  [ -n "$enet_sha" ] && fetch_tarball cgutman/enet "$enet_sha" third-party/moonlight-common-c/enet
fi

echo "DONE. Populated submodules under third-party/."
