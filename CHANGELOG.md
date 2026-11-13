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

- **Reiter „Anlage".** Erkennt die Konfiguration einer Wärmepumpe aus
  Scan und einer einmaligen Sondierung (Rollen der Steuergeräte,
  Sensorstatus `not_available` gegen `no_error`, Konfigurations-Datenpunkte)
  und zeichnet Kältekreis und Hydraulik nach dem Vorbild der
  Viessmann-Serviceansicht mit Live-Werten: Lüfter, Verdampfer,
  Expansionsventil, Verdichter mit Saug- und Hochdruckseite, Vierwegeventil,
  Verflüssiger, Pumpe, Vor-/Rücklauf, Warmwasser, Heizstab, Puffer,
  Heizkreise. Teile, die die Anlage nicht hat, fehlen im Bild. Den Modellnamen
  gibt der Bus nicht her (nur Seriennummer und Nennleistung). Die Sondierung
  liegt als `/data/plant.json` auf dem Gerät und wandert mit der Sicherung.
  Für Home Assistant erzeugt der Reiter dasselbe Bild als
  Picture-Elements-Karte: Hintergrund `/plant.svg` vom Gerät, Werte aus den
  per Discovery angelegten Sensoren, YAML zum Einfügen.

- **Schreibgrenzen je Datenpunkt.** Neue Spalte „Grenzen" in der
  Datenpunkttabelle (`10..60`, `..60`, `Comfort: 10..60`; Feld `range` in
  `points.json`). Home Assistant bekommt den Bereich als `min`/`max` seiner
  Number-Entität statt der Feldbreite (bisher -3276,8 bis 3276,7 für einen
  Temperatur-Sollwert), und jeder Schreibbefehl außerhalb wird abgelehnt,
  auch per MQTT, API und Weboberfläche (#21).

- **Speicher laden mit fester Leistung.** Der Netzsollwert (DID 2188) ist
  der Bezug am Netzanschluss, nicht die Ladeleistung: das Haus wird zuerst
  versorgt, nur der Rest lädt. Bei -2000 W und 1550 W Hausverbrauch kamen
  457 W in der Batterie an. Neu neben dem gehaltenen Netzbezug: „Speicher
  laden mit" in Watt; die Firmware liest alle zehn Sekunden Netzleistung
  (DID 1603) und Speicherleistung (DID 1836) und führt den Sollwert nach,
  höchstens 1500 W je Schritt, nie über 6000 W Bezug, nie Einspeisung. Per
  `POST /api/grid` mit `charge`, auf dem Befehlskanal
  `{"mode":"grid","charge":2000,"seconds":1800}`; `/api/status` und
  `<Basis>/hold` zeigen Ziel, beide Messwerte und den Sollwert.
- **Retained je Datenpunkt** (#13, Spoon3er): Häkchen „Ret." in der
  Datenpunkt-Tabelle, Feld `retain` in `points.json`, Standard aus. Der
  Broker behält den letzten Wert, ein neu verbindender Client bekommt ihn
  sofort statt erst nach dem nächsten Intervall. Gilt für JSON und
  geflacht, dort je Teil-Topic. Beim Abwählen, Deaktivieren, Entfernen des
  Häkchens oder Ändern von Topic oder Modus leert das Gerät die alten
  Topics mit einer leeren retained Nachricht (wenn der Broker gerade
  verbunden ist).
- **Favicon** für die Weboberfläche (#11, wunderbaum): zwei gegenläufige
  Leitungen mit Knoten in der Mitte, sein zweiter Vorschlag. Die Flash-Seite
  trägt dasselbe Symbol. Kommt mit dem nächsten `storage.bin`.

### Behoben

- **Scheinbare Bitfehler „Code 0".** Ein Fehlerinterrupt ohne Inhalt im
  Fehlerregister des Controllers wurde als Bitfehler beim Senden gezählt,
  obwohl das Frame durchging und der Fehlerzähler bei null blieb. Solche
  Ereignisse stehen jetzt nur noch in `/api/status` unter `can.uncaptured`
  und nicht mehr als Busfehler in der Karte (#262).
- **Anfragen gingen am internen Bus verloren** (ioBroker.e3oncan #262,
  Analyse von MyHomeMyData): etwa eine von 1500 am internen CAN einer
  Vitodens kam als „transport error" zurück, gefolgt von einem „no response"
  auf die nächste. Zwei Ursachen im Sendepfad. Verlorene Arbitrierung
  zählte als Fehlversuch, und vier davon hintereinander sind in einem Burst
  mit niedrigeren IDs schnell erreicht; sie wird jetzt bis zu 500 ms lang
  einfach wiederholt, nur echte Busfehler zählen gegen die vier Versuche.
  Und lief die Wartezeit auf das Senden ab, gab die Firmware auf, während
  der Treiber das Frame noch per Zeiger auf dem inzwischen verlassenen Stack
  hielt; es ging später mit beliebigem Inhalt raus. Das Frame liegt jetzt
  fest, ein noch ausstehendes wird vor dem nächsten abgewartet.
  `/api/status` zählt die Fälle unter `txTimeouts`; die Fehlermeldung
  unterscheidet „transport error: send failed" von „transport error:
  protocol".
- **Netzladen plus Speichermodus brachte das Gerät zum Absturz.** Mit beiden
  Halts zugleich lief der Schreibpfad (Lesen, Codec, Mischen, Kodieren und
  die MQTT-Meldung) alle zwei Sekunden im Halte-Task, dessen 4 KiB Stack
  dafür nicht reichten: Absturzbericht „Task hold, Stack beschädigt", das
  Gerät blieb bis zum Stromlos-Machen weg. Der Task hat jetzt denselben
  Stack wie Poller und Kommando-Task, die Meldung hält ihre Puffer nicht
  mehr auf dem Stack.

## 0.3.0 — 2026-11-11

Das Update besteht aus zwei Teilen: der Anwendung (OTA über *Anwendung
aktualisieren*) und dem Abbild der Storage-Partition (`storage.bin`, Karte
*Datenpunktdatenbank*), weil die Datenpunktdatenbank und die Weboberfläche
dort liegen. Wer nur die Anwendung einspielt, behält die alte Datenbank und
die alten Seitentexte. Das Abbild löscht Auswahl und Scan-Ergebnis, vorher
also *Sicherung* ziehen und danach zurückspielen.

### Hinzugefügt

- **WireGuard** Unter *Einstellungen → Anbindung* lässt
  sich ein Tunnel zu einem WireGuard-Server einrichten. Broker und
  Fernzugriff im Tunnel-Subnetz laufen allein über die Netzmaske; die
  zuschaltbare Standardroute leitet zusätzlich alles außerhalb des lokalen
  WLANs durch den Tunnel (für Clients in anderen Netzen hinter dem Server).
  Das lokale WLAN bleibt in beiden Fällen direkt erreichbar, damit ein
  Tunnelausfall das Gerät nicht aussperrt. Die Implementierung ist die
  ESPHome-Bibliothek `esphome/wireguard`.
  Endpunkt, Tunnel-Adresse, Netzmaske, Keepalive, privater Schlüssel,
  Gegenstellen-Schlüssel und optional ein Preshared Key stehen im Web-UI;
  `/api/status` liefert Zustand, Endpunkt-IP und das Alter des letzten
  Handshakes. Die Schlüssel sind Teil der Sicherung, und „Alles außer WLAN"
  löscht die WireGuard-Konfiguration mit. Solange WireGuard aus ist, läuft
  kein Tunnel-Task; der Speicher dafür wird erst beim Einschalten belegt.
  Die Standardroute ist per Vorgabe aus. Den Tunnel selbst hat bisher nur
  der Autor des Beitrags (PR #17, NerdyProjects) an seinem Server geprüft;
  auf dem Referenzgerät lief 0.3.0 mit ausgeschaltetem WireGuard.
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
- **CAN-Bit-Timing einstellbar** unter *Einstellungen → Gerät*:
  Abtastzeitpunkt (50 bis 90 %) und SJW (1 bis 4 Quanten), leer heißt
  Standard (250 kBit/s: 20 Quanten, 85 %, SJW 3). Gilt sofort, der
  Controller startet kurz neu. Die Karte „CAN-Bus" zeigt das angewendete
  Timing, `/api/status` unter `can.timing`. Für Anlagen mit Fehlern am
  Frame-Ende (ioBroker.e3oncan #262), um das Timing als Ursache zu prüfen
  oder auszuschließen.
- **Frames verpasst** auf der Karte „CAN-Bus": Frames, die der
  Empfangs-Interrupt nicht mehr ablegen konnte, bisher nur in `/api/status`
  als `rxMissed`. Die Karte erklärt außerdem, was Busfehler, TX/RX und
  verpasste Frames jeweils zählen.

### Geändert

- **Datenpunktdatenbank auf open3e vom 14.10.2026** (Definitionen 20261014,
  vorher 20260705). DID 1593 BufferHysteresis wird dekodiert statt roh
  ausgegeben (Ein- und Ausschaltpunkt in °C, schreibbar); der Name von
  DID 2428 hatte ein Leerzeichen („…FunctionS ettings"), wer den Punkt
  gewählt hat, bekommt nach dem Update ein neues Topic. E3onCAN auf den
  Stand vom 04.11.2026, die E380-Tabelle ist unverändert. Die Datenbank
  liegt auf der Storage-Partition: `storage.bin` einspielen, das löscht
  Auswahl und Scan-Ergebnis.

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

- **Datei-Upload hängte das Gerät auf**, als die 1,7 MB große Datenbank
  über *Weboberfläche aktualisieren* statt als Partitionsabbild kam: kein
  Absturzbericht, kein Neustart, erst Stromlos-Machen half. Der Datei-Weg
  nimmt jetzt nur noch Dateien bis 256 KiB und verweist auf das Abbild.
  Dazu: ein abgebrochener Upload blockierte den Upload-Platz für immer,
  weil die Empfangsschleife bei jedem Socket-Timeout einfach weiter
  wartete; nach 90 s ohne Daten wird jetzt abgebrochen und die halbfertige
  Datei gelöscht, Reste früherer Uploads (`*.up`) räumt der Start weg. Der
  Task-Watchdog startet das Gerät neu, statt nur zu warnen.
- **Rund drei Busfehler je Sekunde am Viessmann-Bus** (ioBroker.e3oncan
  #262, Issue #14). Der ESP-IDF-Treiber setzt die Resynchronisation (SJW)
  bei 250 kBit/s auf ein einziges Zeitquantum von 200 ns. Hinter dem
  Isolator des Waveshare-Boards reicht das nicht: ACK-Slot-, CRC-Delimiter-
  und Bitfehler auf eigenen Sendungen, bei einer sonst fehlerfreien Anlage.
  Eine vierstündige Messreihe mit SJW 3 und 4 bei 75, 80 und 85 % zeigte
  null Fehler, der Rücksprung auf SJW 1 sofort wieder 3,7/s. Die Firmware
  setzt den SJW jetzt auf die volle Länge des zweiten Phasenabschnitts
  (bei 250 kBit/s 3 Quanten); die Einstellung *CAN-Bit-Timing* überschreibt
  das weiterhin.
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
