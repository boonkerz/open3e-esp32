# Host-side tasks. The firmware itself is built with idf.py; see README.md.

PY := .venv/bin/python

.PHONY: install-pull help setup db fixtures test sanitize storage lint fwinfo package site deploy clean

help:
	@echo "make setup     create the Python venv and fetch open3e"
	@echo "make db        regenerate data/o3edb.bin from the open3e sources"
	@echo "make fixtures  regenerate the test vectors from open3e's own codecs"
	@echo "make test      run the host test suite (codec, encode, flatten, ISO-TP)"
	@echo "make sanitize  run the same suite under ASan/UBSan/LeakSanitizer"
	@echo "make storage   assemble the storage partition contents by hand"
	@echo "               (not normally needed: idf.py build does this)"
	@echo "make lint      check includes and cross-module symbols"
	@echo "make fwinfo    show the built firmware's build identity"
	@echo "make site      assemble the browser-flashing site into build/site"
	@echo "make deploy    rsync build/site to the web server"
	@echo ""
	@echo "Firmware:      idf.py set-target esp32s3 && idf.py build flash"

setup:
	python3 -m venv .venv
	$(PY) -m pip install -q udsoncan
	python3 tools/fetch_open3e.py

db:
	$(PY) tools/gen_dpdb.py -o data/o3edb.bin

fixtures:
	$(PY) tools/gen_fixtures.py -o test/fixtures.json
	$(PY) tools/gen_flat_fixtures.py -o test/flat_fixtures.json
	$(PY) tools/gen_encode_fixtures.py -o test/encode_fixtures.json
	$(PY) tools/gen_em_fixtures.py -o test/em_fixtures.json

test:
	$(PY) -m unittest discover -s test -p test_gen_dpdb.py
	$(MAKE) -C test all

sanitize:
	$(MAKE) -C test sanitize

storage:
	$(PY) tools/build_fs.py

lint:
	python3 tools/check_symbols.py

fwinfo:
	@python3 tools/fwinfo.py

# The page people flash from, at esp32can.thomas-peterson.de.
#
# CHANNEL says which slot the binaries land in: `release` is what the site
# offers by default, `dev` is the untested latest. Both can coexist in the same
# tree, which is why the manifest paths carry the channel.
CHANNEL     ?= dev

# Where `make deploy` sends it. Kept out of the repository: it is a machine on
# somebody's home network, and that is nobody else's business. Put them in
# .deploy.mk (gitignored), for example:
#
#     DEPLOY_HOST = root@10.0.0.2
#     DEPLOY_PATH = /var/www/esp32can
-include .deploy.mk

# Firmware and storage in one file, for Einstellungen -> Wartung -> Update.
package:
	$(PY) tools/make_update.py

site:
	$(PY) tools/build_site.py --channel $(CHANNEL)

# tar over ssh rather than rsync: the target does not have rsync, and for a
# tree that is one 4 MB file which changes on every build anyway, delta
# transfer buys nothing worth installing a package for.
#
# Nothing is deleted on the far side on purpose -- this build produced one
# channel, and it has no business removing the other one's binaries.
deploy: site
	@test -n "$(DEPLOY_HOST)" || { echo "DEPLOY_HOST is not set -- see .deploy.mk in the Makefile"; exit 1; }
	tar -C build/site -czf - . | \
	  ssh $(DEPLOY_HOST) "mkdir -p $(DEPLOY_PATH) && tar -C $(DEPLOY_PATH) -xzf -"
	@echo "-> https://esp32can.thomas-peterson.de"

# The server pulls the page from GitHub Actions on its own (site/pull/): the
# runners cannot reach a host on a home network, so pushing from CI never
# worked. This installs script and timer; the token has to be put at
# /etc/esp32can-site/token on the host by hand -- it does not belong in a
# Makefile, and not on this machine either.
install-pull:
	@test -n "$(DEPLOY_HOST)" || { echo "DEPLOY_HOST is not set -- see .deploy.mk in the Makefile"; exit 1; }
	tar -C site/pull -czf - . | ssh $(DEPLOY_HOST) '\
	  set -e; d=$$(mktemp -d); tar -C $$d -xzf -; \
	  install -m 755 $$d/esp32can-site-pull.py /usr/local/bin/; \
	  install -m 644 $$d/esp32can-site-pull.service $$d/esp32can-site-pull.timer /etc/systemd/system/; \
	  install -d -m 700 /etc/esp32can-site /var/lib/esp32can-site; rm -rf $$d; \
	  systemctl daemon-reload; systemctl enable --now esp32can-site-pull.timer; \
	  systemctl list-timers esp32can-site-pull.timer --no-pager'

clean:
	$(MAKE) -C test clean
	rm -rf build storage_image storage_image.bin data
