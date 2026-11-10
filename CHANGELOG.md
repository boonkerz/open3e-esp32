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
- **Busfehler nach Art, Richtung und Stelle** auf der Karte „CAN-Bus":
  CRC, Bit, Stuff, Form, ACK und andere getrennt gezählt, dazu „Arbitrierung
  verloren" als eigene Zeile, „Fehler beim Empfang / Senden" und die
  Stelle im Frame, an der der Controller den Fehler sah. Die Firmware liest
  dafür das Fehlerregister des Controllers selbst; der ESP-IDF-Treiber
  reicht nur vier Flags weiter und lässt CRC-Fehler ganz weg. Der
  ESP-IDF-Treiber zählt verlorene Arbitrierung in seiner Fehlersumme mit,
  obwohl es kein Fehler ist, sondern ein anderer Knoten, der im selben Bit
  angefangen hat; es kann nur passieren, während das Gerät selbst sendet.
  Die bisherige Summe „Busfehler" stieg deshalb genau dann, wenn der Poller
  lief, und sah nach Verkabelung aus. `/api/status` liefert `arbLost`,
  `crcErrors`, `bitErrors`, `stuffErrors`, `formErrors`, `ackErrors`,
  `otherErrors`, `rxErrors`, `txErrors`, `errSegments` und `txRetries`
  neben dem unveränderten `busErrors`.
- **Bus-Diagnose zählt Busfehler beim Mithören.** Der Listen-Only-Sweep
  zeigt je Bitrate, wie viele Busfehler der Controller in den 2 s sah, in
  denen er nichts auf den Bus treibt, nicht einmal das ACK-Bit. Fehler, die
  dort auftreten, liegen auf der Leitung oder im Empfangsweg; Fehler, die
  nur im Normalbetrieb zählen, entstehen durch das eigene Treiben des
  Geräts. Das gilt nur mit einem dritten Knoten, der quittiert: Hängen nur
  Steuerung und Gateway am Bus, wiederholt die Steuerung ohne ACK jedes
  Frame im Dauerlauf. Die Diagnose erkennt das an tausendfach gleichen
  Frames hintereinander und sagt es, statt die Leitung zu verdächtigen.
  `/api/candiag` liefert je Bitrate `errors` und `repeats`.
- **CAN-Bit-Timing einstellbar** unter *Einstellungen → System*:
  Abtastzeitpunkt (50 bis 90 %) und SJW (1 bis 4 Quanten), leer heißt
  Treiberstandard (250 kBit/s: 20 Quanten, 85 %, SJW 1). Gilt sofort, der
  Controller startet kurz neu. Die Karte „CAN-Bus" zeigt das angewendete
  Timing, `/api/status` unter `can.timing`. Für Anlagen mit Fehlern am
  Frame-Ende (ioBroker.e3oncan #262), um das Timing als Ursache zu prüfen
  oder auszuschließen.
- **Frames verpasst** auf der Karte „CAN-Bus": Frames, die der
  Empfangs-Interrupt nicht mehr ablegen konnte, bisher nur in `/api/status`
  als `rxMissed`. Die Karte erklärt außerdem, was Busfehler, TX/RX und
  verpasste Frames jeweils zählen.

### Geändert

- **Frames werden nach verlorener Arbitrierung oder einem Busfehler
  wiederholt**, bis zu dreimal. Bisher sendete der Controller jedes Frame
  genau einmal; traf ein Busfehler die Anfrage oder fing ein anderer Knoten
  im selben Bit an, war sie verloren und erschien eine Sekunde später als
  „no response", obwohl die ECU sie nie gesehen hatte (ioBroker.e3oncan
  #262). Die Wiederholung liegt in der Firmware, nicht im Controller: der
  klassische TWAI-Controller des ESP32-S3 kennt nur „einmal" oder „endlos",
  und „endlos" würde ohne Gegenstelle am Bus den Sendepfad blockieren.
  Leere Adressen beim Scan geben weiterhin sofort auf.

### Behoben

- **Codec-Bäume liegen im PSRAM** (#20, konairius). Die kompilierten
  Datenpunkt-Beschreibungen jedes Poll-Punkts lagen im internen RAM, weil
  kleine Allokationen dort landen; mit 29 ZigBee-Punkten zu je etwa 8 KB
  blieben 16 Byte frei und der WLAN-Task stürzte ab, während der PSRAM
  leer war. Knoten, Namen und Kindlisten kommen jetzt aus dem PSRAM, ohne
  PSRAM greift der Standard-Heap. Mit 91 UDS- und 7 E380-Punkten steigt
  der freie interne Speicher von 108 auf 137 KB.
- **Doku zur Masse war falsch.** README, Statusseite und die Texte unter
  `docs/` behaupteten, R31 überbrücke die galvanische Trennung und die
  Board-Masse sei der CAN-Bezugspunkt, der an die Busmasse müsse. Laut
  Schaltplan verbindet R31 die CAN-Masse `SGND` nur mit `EARTH`, dem
  Ableitpunkt der RS485-Schutzdioden; der CAN-Teil ist getrennt, eine
  Busmasse an GND erreicht den Transceiver nicht (#14, Ersthelfer). Die
  Anleitung lautet jetzt: nur CAN-H und CAN-L anschließen.
- **„Busfehler" sprang nach der Bus-Diagnose zurück**, weil der Zähler des
  Treibers mit jedem neuen Controller-Start bei null anfängt, während die
  Aufschlüsselung weiterzählte. Die Summe läuft jetzt über Diagnose und
  Timing-Wechsel hinweg durch; der Sweep selbst zählt nicht mit.
- **„Frames verpasst" zählte weiter**, wenn gerade keine Anfrage lief: die
  Kennung für „keine Anfrage" war die CAN-ID 0, und eine Anlage sendete
  Frames mit dieser ID. Der Leerwert ist jetzt eine ID, die kein Frame
  tragen kann.
- **Fehlerstellen ohne Namen** zeigten den Zähler 0, die Summenbildung fand
  den Roh-Namen nicht wieder.
- **„Frames verpasst" zählte Broadcast-Frames mit.** Jedes Frame, das kein
  Listener wollte, landete in der Warteschlange für UDS-Antworten, die nur
  während einer Anfrage geleert wird. An einem echten Bus füllte sie der
  Broadcast-Verkehr der Anlage, und jedes weitere Frame zählte als
  verpasst: 34000 in zwei Stunden an einer Vitodens, ohne eine einzige
  verlorene Antwort (ioBroker.e3oncan #262). Jetzt kommt nur noch die
  Antwort der laufenden Anfrage in die Warteschlange, `rxMissed` zählt
  allein diese.
- **Fehlerstellen ohne Namen** erschienen gar nicht. Der Controller
  meldet Segment-Codes, für die der Treiber-Header keinen Namen hat;
  die fielen aus der Aufstellung heraus, sodass ihre Summe nicht zur
  Zahl der Fehler passte. `errSegments` führt sie als `seg<Code>`, die
  Karte als „Code <n>".
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
