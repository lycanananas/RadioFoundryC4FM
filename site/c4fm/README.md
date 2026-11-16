<!-- SPDX-License-Identifier: MIT -->
# C4FMFoundry web site

The fork's site: a home page about C4FMFoundry, the web installer and the Codeplug Editor, for the Baofeng DM-32UV and
its C4FM release image only. English only.

| Part | Where |
|---|---|
| Generator | `site/c4fm/build.py`: RadioFoundry's generator (`site/rfoundrysite`) with one board, one variant, three pages |
| Templates | `site/c4fm/templates/`, laid over `site/templates/` (same name wins) |
| Texts | `site/c4fm/en.overrides.json`, laid over `site/i18n/en.json` |
| Installer | `flash.html` sets `data-api=""`: no build service. The installer installs the release image in `firmware/`, has no feature picker or version links, and sends nothing (no flash log, reports or samples) |
| Image | `site/c4fm/docker/`: nginx serving the built site on port 80, TLS left to a reverse proxy |

## Build the image

From the checkout's root, with the C-SKY toolchain and a Python that has `site/requirements.txt`:

```
PYTHON=../venv-site/bin/python C4FM_SITE_URL=https://c4fm.example.org/ site/c4fm/docker/build-image.sh
```

| Variable | Default | |
|---|---|---|
| `VERSION` | this month, `vYY.M` | release name ([tools/release_version.py](../../tools/release_version.py)); the next one that month is `vYY.M.1` |
| `C4FM_SITE_URL` | `http://localhost:8080/` | the public address, for link previews |
| `SKIP_FIRMWARE` | `0` | `1`: package the image already in `build/dm32-c4fm` |
| `EXPORT` | `0` | `1`: also write `build/c4fmfoundry-site-<version>.tar` with skopeo |

The script builds `make BOARD=dm32 VARIANT=c4fm` (no warnings allowed, then `stack-check`), packages it with
`tools/release.py` (the codec-less image and its manifest), builds the site with `--strict`, copies only the site and
`nginx.conf` into `build/c4fm-docker/` and runs `docker build` there. It tags `c4fmfoundry-site:<version>` and
`:latest`.

The AMBE codec never reaches the image: `release.py` refuses an image that carries it, and the script refuses a
staging directory with a `*codec*.bin` in it. The installer takes the codec from the user's own radio.

Try it locally: `docker run --rm -p 8080:80 c4fmfoundry-site:latest`, then http://localhost:8080/ (WebSerial works on
localhost).

## Move it to a server

As a file (skopeo, no registry):

```
skopeo copy docker-daemon:c4fmfoundry-site:v26.11 docker-archive:c4fmfoundry-site-v26.11.tar:c4fmfoundry-site:v26.11
scp c4fmfoundry-site-v26.11.tar server:
ssh server 'docker load -i c4fmfoundry-site-v26.11.tar'
```

Or straight over SSH: `docker save c4fmfoundry-site:v26.11 | gzip | ssh server 'gunzip | docker load'`.

## Run it on the server

`site/c4fm/docker/compose.yml` runs the image read-only on `127.0.0.1:8080`:

```
TAG=v26.11 docker compose -f compose.yml up -d
```

The reverse proxy serves it on https: WebSerial and WebUSB need a secure page. Caddy, for example:

```
c4fm.example.org {
	reverse_proxy 127.0.0.1:8080
}
```
