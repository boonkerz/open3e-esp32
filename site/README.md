# site/

Die Seite unter <https://esp32can.thomas-peterson.de>, über die sich die
Firmware direkt aus dem Browser aufspielen lässt (Web Serial API).

Nicht unter `web/` ablegen: `tools/build_fs.py` packt `web/` rekursiv in die
4-MB-Datenpartition des Geräts. Diese Seite gehört auf einen Webserver, nicht
in den Flash der Heizungssteuerung.

## Aufbau nach dem Ausrollen

    index.html
    manifest-release.json     aus tools/build_site.py, Version aus dem Image
    manifest-dev.json
    bin/release/*.bin         die fünf Abschnitte des jeweiligen Standes
    bin/dev/*.bin
    doku/                     die Dokumentation: docs/ mit MkDocs gerendert (mkdocs.yml)

## Offsets

Aus `build/flasher_args.json`; in JSON zwingend dezimal, weil JSON keine
Hexadezimalzahlen kennt.

| Datei | Offset | dezimal |
|---|---|---|
| `bootloader.bin` | `0x0` | 0 |
| `partition-table.bin` | `0x8000` | 32768 |
| `ota_data_initial.bin` | `0xF000` | 61440 |
| `open3e-gateway.bin` | `0x20000` | 131072 |
| `storage.bin` | `0x820000` | 8519680 |

Ein falscher Offset erzeugt kein Fehlerbild, sondern ein Gerät, das nicht
startet. Nach jeder Änderung an `partitions.csv` gehören diese Zahlen geprüft.

## Bauen und ausrollen

Den Normalfall erledigt GitHub Actions: jeder Push auf `main` baut die
Firmware, lässt die Host-Tests laufen und legt die fertige Seite als Artefakt
`site-dev` ab; ein Tag `v*` genauso als `site-release`. Der Webserver holt
sich diese Artefakte selbst (`pull/`, unten) — GitHubs Runner erreichen eine
Maschine im Heimnetz nicht, und ein Deploy-Schritt, der deshalb stumm
übersprungen wird, hat die Seite einmal einen Monat lang veralten lassen (#3).

Von Hand, etwa um einen lokalen Stand zu testen:

    make site      # Firmware muss gebaut sein; site/ nach build/site/ zusammenstellen
    make deploy    # das Ergebnis per ssh auf den Webserver schieben

`CHANNEL=release make site` legt den Build unter `bin/release/` ab, Standard
ist `dev`. Der nächste Lauf des Timers überschreibt einen manuellen Stand nur,
wenn GitHub inzwischen einen neueren erfolgreichen Lauf hat.

### pull/ — der Server holt sich die Seite

    esp32can-site-pull.py        fragt die GitHub-API nach dem neuesten erfolgreichen
                                 Lauf je Kanal, lädt site-<kanal>.zip, entpackt es
    esp32can-site-pull.service   oneshot, läuft als root mit ProtectSystem=strict
    esp32can-site-pull.timer     alle 5 Minuten, 2 Minuten nach dem Booten

Installieren mit `make install-pull` (nutzt `DEPLOY_HOST` aus `.deploy.mk`).
Das Skript erwartet ein GitHub-Token in `/etc/esp32can-site/token` (Modus
0600): ein *fine-grained personal access token* für dieses Repository mit
der einzigen Berechtigung **Actions: Read** — Artefakte lassen sich auch bei
öffentlichen Repositories nur angemeldet herunterladen. Es merkt sich die
zuletzt installierte Run-ID in `/var/lib/esp32can-site/state.json` und lädt
nur, wenn sich die geändert hat.

    journalctl -u esp32can-site-pull.service -n 20    # was zuletzt passiert ist
    systemctl start esp32can-site-pull.service        # jetzt holen statt warten

Beim Einspielen wird erst `bin/<kanal>/` per Umbenennen getauscht und dann das
Manifest ersetzt — ein Browser sieht nie ein Manifest, dessen Binärdateien
noch fehlen. Die Dateien des anderen Kanals bleiben unberührt.

Die Dokumentation unter `doku/` gibt es nur einmal, für beide Kanäle: sie
wird als ganzer Baum getauscht, der zuletzt geholte Lauf gewinnt. Ein
Artefakt ohne `doku/` lässt die vorhandene stehen.

## Dokumentation

`docs/*.md` sind die Kapitel, `mkdocs.yml` legt Navigation und Aussehen
fest, `docs/changelog.md` bindet `CHANGELOG.md` ein. Lokal ansehen:

    make docs-setup    # einmalig: mkdocs-material in .venv/
    make docs-serve    # http://127.0.0.1:8000, lädt bei Änderungen neu

`make site` rendert sie nach `build/site/doku/`, wenn der Generator
installiert ist; GitHub Actions verlangt ihn (`--docs`) und bricht bei
toten Links ab.

## Voraussetzungen auf dem Server

Web Serial verlangt **HTTPS** — ohne gültiges Zertifikat bleibt der Knopf
wirkungslos. Einzige Ausnahme ist `http://localhost`, weshalb sich die Seite
lokal ohne Zertifikat testen lässt:

    make site && (cd build/site && python3 -m http.server 8000)

Die `.bin`-Dateien müssen unverändert ausgeliefert werden.
