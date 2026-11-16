#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Builds the C4FM firmware, packages it as a release, builds the fork's site with it and wraps the site in a local
# Docker image (site/c4fm/README.md). Run from the checkout's root.
#
#   site/c4fm/docker/build-image.sh
#   VERSION=v26.11.1 C4FM_SITE_URL=https://c4fm.example.org/ site/c4fm/docker/build-image.sh
#   EXPORT=1 site/c4fm/docker/build-image.sh          also writes build/c4fmfoundry-site-<version>.tar (skopeo)
#
# VERSION: the release name, vYY.M[.N] (tools/release_version.py); default this month's. SKIP_FIRMWARE=1 packages the
# image already in build/dm32-c4fm. PYTHON: a Python with site/requirements.txt (default python3). CSKY: the
# toolchain's bin directory (default ~/toolchains/csky-elf/bin).
set -eu

[ -f Makefile ] && [ -d core/c4fm ] || { echo "run from the C4FM checkout's root" >&2; exit 2; }
VERSION=${VERSION:-v$(date +%y).$(date +%-m)}
PYTHON=${PYTHON:-python3}
CSKY=${CSKY:-$HOME/toolchains/csky-elf/bin}
SITE_URL=${C4FM_SITE_URL:-http://localhost:8080/}
IMAGE=c4fmfoundry-site
DIST=build/c4fm-dist
STAGE=build/c4fm-docker
PATH="$CSKY:$PATH"
export PATH

python3 tools/release_version.py check "$VERSION"
if [ "${SKIP_FIRMWARE:-0}" != 1 ]; then
	make -j"$(nproc)" BOARD=dm32 VARIANT=c4fm >build/dm32-c4fm.log 2>&1 || { tail -30 build/dm32-c4fm.log; exit 1; }
	! grep ": warning:" build/dm32-c4fm.log || { echo "the firmware build has warnings" >&2; exit 1; }
	make BOARD=dm32 VARIANT=c4fm stack-check | tail -1
fi
[ -z "$(git status --porcelain -- core drivers platform boards hal 2>/dev/null)" ] || echo "note: firmware with uncommitted changes"

# the codec-less image and its manifest (release.py refuses an image with the codec in it)
rm -rf "$DIST"
python3 tools/release.py --board dm32 --variant c4fm --version "$VERSION" --out "$DIST"
"$PYTHON" site/c4fm/build.py --strict --firmware "$DIST" --out build/site-c4fm --site-url "$SITE_URL"

# the image's context: the site and the server's configuration only
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -r build/site-c4fm "$STAGE/site"
cp site/c4fm/docker/Dockerfile site/c4fm/docker/nginx.conf "$STAGE/"
if find "$STAGE" -name '*codec*.bin' | grep -q .; then
	echo "a codec file is in the site: refusing to build the image" >&2
	exit 1
fi
GIT=$(git rev-parse --short=8 HEAD)
docker build -q -t "$IMAGE:$VERSION" -t "$IMAGE:latest" --build-arg VERSION="$VERSION" --build-arg GIT="$GIT" "$STAGE"
echo "image: $IMAGE:$VERSION ($GIT)"

if [ "${EXPORT:-0}" = 1 ]; then
	out=build/$IMAGE-$VERSION.tar
	rm -f "$out"
	skopeo copy "docker-daemon:$IMAGE:$VERSION" "docker-archive:$out:$IMAGE:$VERSION"
	echo "archive: $out ($(du -h "$out" | cut -f1))"
fi
