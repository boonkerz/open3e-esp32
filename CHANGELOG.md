# Changelog

Was sich zwischen zwei Versionen für Nutzer geändert hat. Die Nummer folgt
[Semantic Versioning](https://semver.org/lang/de/) und steht in `version.txt`;
ein Release ist ein Git-Tag `v<Version>` auf dem Commit, der diese Datei und
`version.txt` anhebt. Die Raw-API für externe Integrationen hat eine eigene
Versionsnummer (`rawApiVersion`), siehe `docs/raw-gateway-api.md`.

Vor 0.2.0 gab es keine Versionsnummern; 0.1.0 ist rückwirkend der Stand, der
bis dahin auf der Flash-Seite lag.

## Unveröffentlicht

### Hinzugefügt

- **Schaltausgänge** (#5, TheSmartGerman): GPIO1 und GPIO2 lassen sich je
  Pin als Ausgang betreiben, etwa für ein Relais an den SG-Ready-Klemmen.
  Zustand auf `<Basis>/output/<name>`, Befehl `ON`/`OFF` auf `…/set`, in
  Home Assistant ein Schalter, `POST /api/output`, Befehlskanal-Modus
  `output`, KNX-Meldung beim Schalten. Nach einem Neustart ist ein Ausgang
  aus. Die Einstellungen bekommen je Pin `mode` und `activeLow`; ein
  Backup ohne die Felder bleibt ein Eingang.
- **Raw-Reads mit eigener Frist:** `GET /api/rawread` wartet 2,5 s statt 1 s
  auf die Antwort der ECU, ein Client kann per `timeout=<ms>` (200 bis 7000)
  seine eigene Frist durchreichen. `/api/status` zählt die externen
  Zugriffe unter `rawRead` (gelesen, ohne Antwort, Fehler, geschrieben),
  die Statusseite zeigt sie als „Raw-Reads extern". `rawApiVersion` bleibt
  `1`.
- **Frames verpasst** auf der Karte „CAN-Bus": Frames, die der
  Empfangs-Interrupt nicht mehr ablegen konnte, bisher nur in `/api/status`
  als `rxMissed`. Die Karte erklärt außerdem, was Busfehler, TX/RX und
  verpasste Frames jeweils zählen.

### Geändert

- **Anfrage-Frames werden bei einem Busfehler wiederholt**, bis zu dreimal
  in Hardware. Bisher sendete der Controller jedes Frame genau einmal; traf
  ein Busfehler die Anfrage, war sie verloren und erschien eine Sekunde
  später als „no response", obwohl die ECU sie nie gesehen hatte
  (ioBroker.e3oncan #262). Leere Adressen beim Scan geben weiterhin sofort
  auf.

### Behoben

- **Collect-Listener** gleicht die konfigurierten CAN-IDs exakt ab statt
  alles zwischen kleinster und größter ID zu nehmen (#10, MyHomeMyData).
  Vorher schluckte der Listener auch UDS-Anfragen und -Antworten, deren ID
  in die Spanne fiel, etwa 0x680/0x690 bei Collect-IDs von 0x250 bis 0x693;
  Poller und `rawread` meldeten dann „no response", und weil alle
  UDS-Zugriffe hintereinander auf den Bus warten, stockte die ganze Queue
  für etwa zehn Sekunden.

## 0.2.0 — 2026-10-06

Raw-API-Version: 1.

### Hinzugefügt

- **Raw-API für externe Integrationen** (#1, MyHomeMyData): `GET /api/rawread`
  und `POST /api/rawwrite` für rohe UDS-Bytes, MQTT-Relay konfigurierter
  CAN-IDs (`rawCanIds`), retained `open3e/status`, `rawApiVersion` und
  `rawWriteEnabled` in Status und Einstellungen. Erste Gegenseite ist
  ioBroker.e3oncan.
- **Versionsnummer** (#4): `version.txt`, Anzeige als `0.2.0+<commit>`,
  getrenntes Feld `version` in `/api/status`, Raw-API-Version auf der
  Statusseite. Ein Release-Tag muss zur Datei passen, der Workflow prüft das.
- **Kontakteingänge** auf GPIO1 und GPIO2 (SH1.0-Stecker) als MQTT-Sensoren,
  mit Entprellung und wählbarem Namen.
- **KNX:** ein Gruppentelegramm, wenn ein Kontakteingang schaltet.
- **evcc:** die passende Konfiguration erzeugen, statt EEBUS auf dem Gerät
  nachzubauen.
- **Netzladen und Halten:** Netzsollwert gegen die Regelung halten,
  Speicher-Betriebsart (stillstehen, nur laden, nur entladen), in Home
  Assistant als Schalter mit Leistung, Dauer und Restzeit.
- **Absturzberichte** im Flash, auf der Statusseite lesbar statt nur am
  seriellen Kabel.
- **Lizenz:** Apache-2.0 mit `LICENSE` und `NOTICE`, von der Flash-Seite
  verlinkt.

### Geändert

- Einstellungen in fünf Bereiche geteilt, mit einem Speichern-Knopf.
- Die Oberfläche sperrt Abschnitte, die die laufende Firmware nicht kennt,
  statt sie wirkungslos anzubieten.
- Verworfene Frames werden in allen drei Empfängern gezählt.
- Mehr gleichzeitige HTTP-Verbindungen.

### Behoben

- Raw-Relay-Queue für Mehrfach-Frame-Bursts dimensioniert (#6, MyHomeMyData).
- `rawCanIds`-Puffer reichte nicht für den vollen Satz aus Energiezähler und
  Collect-IDs.
- Halten: ein Stopp-Befehl vor dem ersten Halten stürzte ab.
- hold.h: Reglertakt gemessen statt geraten, Adresse ist anlagenabhängig.

## 0.1.0 — 2026-09-05

Erster veröffentlichter Stand, Commit `008b37d`. Nie getaggt.

- Viessmann-E3-Datenpunkte über CAN (UDS/ISO-TP) lesen und schreiben, nach
  MQTT mit Home-Assistant-Discovery veröffentlichen.
- Datenpunkt-Datenbank aus open3e erzeugt, Weboberfläche auf dem Gerät mit
  Scan, Datenpunktauswahl, Diagnose und drei Update-Pfaden (Firmware,
  Oberfläche, Datenbank).
- Flash-Seite, über die sich die Firmware direkt aus dem Browser aufspielen
  lässt.
- Eigene Beschriftungen für Zahlenwerte als Auswahlliste, Broadcast-Kanal für
  die Live-Ansicht.
