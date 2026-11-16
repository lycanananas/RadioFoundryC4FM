# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The C4FM fork's web site as a static nginx image (site/c4fm/README.md). The context is the staging directory
# site/c4fm/docker/build-image.sh fills: the built site and this directory's nginx.conf, nothing else from the tree.
FROM nginx:1.28-alpine

ARG VERSION=dev
ARG GIT=unknown
LABEL org.opencontainers.image.title="C4FMFoundry site" \
      org.opencontainers.image.description="Web installer and codeplug editor for the DM-32UV C4FM firmware" \
      org.opencontainers.image.source="https://github.com/lycanananas/RadioFoundryC4FM" \
      org.opencontainers.image.licenses="GPL-2.0-or-later" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${GIT}"

RUN rm -rf /usr/share/nginx/html/* /etc/nginx/conf.d/default.conf
COPY nginx.conf /etc/nginx/conf.d/site.conf
COPY site/ /usr/share/nginx/html/

EXPOSE 80
HEALTHCHECK --interval=30s --timeout=3s --retries=3 CMD wget -q -O /dev/null http://127.0.0.1/ || exit 1
