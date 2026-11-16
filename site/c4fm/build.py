#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Builds the C4FM fork's web site: a home page about the fork, the web installer and the codeplug editor, for one radio
and its release image only (site/c4fm/README.md).

    python3 site/c4fm/build.py --firmware build/c4fm-dist          build/site-c4fm/
    python3 site/c4fm/build.py --firmware DIR --site-url https://c4fm.example.org/ --serve

It reuses RadioFoundry's generator (site/rfoundrysite) with the templates in site/c4fm/templates laid over
site/templates, and site/c4fm/en.overrides.json over the English catalog. Only English is built. The installer runs
without RadioFoundry's build service (the flash page's data-api=""): it installs the image in --firmware and sends
nothing anywhere.
"""
import argparse
import functools
import html
import http.server
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from rfoundrysite import build as base, device, i18n, repo, shots, social, tmpl, toolzip  # noqa: E402

DEFAULT_REPO = "https://github.com/lycanananas/RadioFoundryC4FM"
UPSTREAM = "https://radiofoundry.org/"


class OverlayTemplates(tmpl.Templates):
	"""site/c4fm/templates first, then site/templates."""

	def __init__(self, overlay, directory, translate):
		super().__init__(directory, translate)
		self.overlay = overlay

	def source(self, name):
		if name not in self._cache:
			p = os.path.join(self.overlay, name + ".html")
			if os.path.isfile(p):
				with open(p, encoding="utf-8") as f:
					self._cache[name] = f.read()
		return super().source(name)


def english_catalog(work):
	"""A catalog directory with English only: site/i18n/en.json and the fork's overrides."""
	if os.path.isdir(work):
		shutil.rmtree(work)
	os.makedirs(work)
	shutil.copy2(repo.path("site", "i18n", "en.json"), work)
	shutil.copy2(os.path.join(HERE, "en.overrides.json"), work)
	with open(os.path.join(work, "languages.json"), "w", encoding="utf-8") as f:
		json.dump({"default": "en", "languages": [{"code": "en", "name": "English", "dir": "ltr", "locale": "en-US"}]}, f)
	return work


class ForkSite(base.Site):
	def __init__(self, out, repo_url, firmware, board, variant, site_url, log=print):
		base.SITE_URL = site_url	# page() reads these module names for absolute URLs and the help links
		base.COMMUNITY_URL = ""
		super().__init__(out, repo_url, "main", firmware=firmware, log=log,
				 i18n_dir=english_catalog(os.path.join(os.path.dirname(os.path.abspath(out)), "site-c4fm-i18n")))
		self.tpl = OverlayTemplates(os.path.join(HERE, "templates"), repo.path("site", "templates"), self.i18n.t)
		self.sentry_dsn = ""
		self.only_board, self.only_variant, self.site_url = board, variant, site_url

	def warn(self, msg):
		# the fork's switch has no row in RadioFoundry's feature matrix: it gets its title and group in load()
		if msg != "feature C4FM has no row in docs/feature-matrix.md":
			super().warn(msg)

	def load(self):
		super().load()
		c4fm = self.fmap.get("C4FM")
		if c4fm:
			c4fm.update(group="Radio and voice", title="C4FM (Yaesu System Fusion DN voice)")
			self.groups = [g for g in self.groups if any(f["group"] == g for f in self.features)]
		self.boards = [b for b in self.boards if b["id"] == self.only_board]
		if not self.boards:
			raise repo.SourceError("no board %s in boards/" % self.only_board)
		self.planned = []
		self.releases = [(m, img) for m, img in self.releases
				 if m["board"] == self.only_board and m.get("variant", "full") == self.only_variant]
		if not self.releases:
			raise repo.SourceError("no %s %s release image in %s (tools/release.py --board %s --variant %s)" % (
				self.only_board, self.only_variant, self.firmware, self.only_board, self.only_variant))
		self.release, self.beta = base.release_and_beta([m["version"] for m, _ in self.releases], "")

	def nav(self, lang, home, current):
		items = [("flash", "flash/index.html"), ("codeplug", "codeplug/index.html")]
		return "".join('<a href="%s%s"%s>%s</a>' % (home, href, ' aria-current="page"' if key == current else "",
							    html.escape(self.t(lang, "nav." + key)))
			       for key, href in items)

	def json_data(self):
		data = super().json_data()
		data["site.json"]["upstream"] = UPSTREAM
		data["site.json"]["fork"] = {"board": self.only_board, "variant": self.only_variant}
		return data

	def home(self, lang):
		b = self.boards[0]
		m = self.releases[-1][0]
		ctx = {"flagship_id": b["id"], "flagship_name": b["name"], "release": self.release, "git": m.get("git", ""),
		       "upstream": UPSTREAM}
		self.drawn(b, b["frame_geometry"], shots.css_px(".hero-device img", "max-height") * b["frame_geometry"]["aspect"])
		self.page(lang, "index.html", self.t(lang, "home.title"), self.tpl.render("home", ctx, lang), "home")

	def build(self):
		self.load()
		if os.path.isdir(self.out):
			shutil.rmtree(self.out)
		os.makedirs(self.out)
		shutil.copytree(repo.path("site", "static"), os.path.join(self.out, "static"))
		self.version_imports(os.path.join(self.out, "static", "js"))
		shutil.rmtree(os.path.join(self.out, "static", "img", "radios", "src"), ignore_errors=True)
		# the fork's screenshots (docs/ux/c4fm) for the home page
		shutil.copytree(repo.path("docs", "ux", "c4fm"), os.path.join(self.out, "static", "img", "c4fm"))
		for b in self.boards:
			img = device.render(b, b["name"])
			self.write("static/img/radios/%s-frame.svg" % b["id"], img["frame"])
			b["frame_geometry"] = img["geometry"]
			b["render"] = img
			b["look_renders"] = {}
		with open(os.path.join(self.out, "static", "img", "social.png"), "wb") as f:
			f.write(social.png(social.card(self.boards)))
		self.tool = toolzip.write(os.path.join(self.out, "downloads"))
		for name, obj in self.json_data().items():
			self.write("data/" + name, json.dumps(obj, indent=1, ensure_ascii=False) + "\n")
		self.copy_firmware()
		lang = self.i18n.default
		self.home(lang)
		self.flash_page(lang)
		self.codeplug_page(lang)
		self.not_found(lang)
		for b in self.boards:
			self.write("static/img/radios/%s.svg" % b["id"], device.boot(b["render"], self.shots.scaled(b["gallery"], "boot")))
		self.shots.write(self.out)
		self.check_links()
		missing = self.i18n.unused_check()
		if missing:
			self.warn("keys missing from the English catalog: %s" % ", ".join(missing))
		return self.warnings


def main(argv=None):
	ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	ap.add_argument("--out", default=repo.path("build", "site-c4fm"))
	ap.add_argument("--firmware", required=True, help="the release image and manifest (tools/release.py --out)")
	ap.add_argument("--board", default="dm32")
	ap.add_argument("--variant", default="c4fm")
	ap.add_argument("--repo-url", default=os.environ.get("C4FM_REPO_URL", DEFAULT_REPO))
	ap.add_argument("--site-url", default=os.environ.get("C4FM_SITE_URL", "http://localhost:8000/"),
			help="the site's public address, for link previews")
	ap.add_argument("--strict", action="store_true", help="exit 1 when the build warns")
	ap.add_argument("--serve", action="store_true", help="serve the result on localhost")
	ap.add_argument("--port", type=int, default=8000)
	args = ap.parse_args(argv)
	try:
		import markdown_it, resvg_py  # noqa: F401
	except ImportError:
		print("site/c4fm/build.py: markdown-it-py or resvg_py is missing: pip install -r site/requirements.txt", file=sys.stderr)
		return 2
	site_url = args.site_url if args.site_url.endswith("/") else args.site_url + "/"
	site = ForkSite(args.out, args.repo_url, args.firmware, args.board, args.variant, site_url)
	try:
		warnings = site.build()
	except repo.SourceError as e:
		print("site/c4fm/build.py: %s" % e, file=sys.stderr)
		return 1
	print("site: %s, firmware %s (%d warning%s)" % (args.out, site.release, len(warnings), "" if len(warnings) == 1 else "s"))
	if warnings and args.strict:
		return 1
	if args.serve:
		handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=args.out)
		print("serving http://localhost:%d/ (Ctrl-C stops)" % args.port)
		try:
			http.server.ThreadingHTTPServer(("localhost", args.port), handler).serve_forever()
		except KeyboardInterrupt:
			pass
	return 0


if __name__ == "__main__":
	sys.exit(main())
