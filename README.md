# open3e Gateway für das Waveshare ESP32-S3-RS485-CAN

Firmware, die Viessmann-E3-Geräte (Vitocal, Vitodens, VX3/Vitocharge) direkt am
CAN-Bus ausliest und nach MQTT bringt — ohne Raspberry Pi, ohne Python, ohne
SocketCAN. Das Board hat einen galvanisch getrennten CAN-Transceiver, WLAN und
läuft an 7–36 V direkt im Heizungsraum.

Ablauf aus Anwendersicht:

1. Erster Start → das Gerät spannt einen WLAN-Hotspot auf, das Captive Portal
   öffnet die Einrichtungsseite von selbst.
2. WLAN auswählen, Passwort eingeben → Neustart, danach erreichbar unter
   `http://open3e.local`.
3. Im Web-UI einen Bus-Scan starten → gefundene ECUs und Datenpunkte erscheinen.
4. Pro Datenpunkt festlegen, ob und auf welches MQTT-Topic er gesendet wird.

Die Topics, Payloads, der LWT und das Kommando-Topic folgen
[open3e](https://github.com/open3e/open3e). Ein bestehendes Broker-Setup oder
Home-Assistant-Template läuft unverändert weiter.

---

## Hardware

| Funktion | GPIO |
|---|---|
| CAN TX (TWAI) | 15 |
| CAN RX (TWAI) | 16 |
| RS485 TX / RX / DE | 17 / 18 / 21 (von dieser Firmware nicht genutzt) |
| Kontakteingang 1 / 2 | 1 / 2 (SH1.0-Stecker, siehe unten) |

Modul ESP32-S3-WROOM-1, 16 MB Flash, 8 MB PSRAM. Bus mit **250 kBit/s**.

**Verkabelung.** CAN-H und CAN-L an den E3-Bus — und die Busmasse an **GND des
Boards**. Wie das in einer Unterverteilung neben der Heizung aussieht, mit
Fotos, Schaltbild und einem bezahlbaren Stecker für die gelbe Buchse 91:
[`docs/aufbau-praxis.md`](docs/aufbau-praxis.md).

Aus dem [Schaltplan](https://files.waveshare.com/wiki/ESP32-S3-RS485-CAN/ESP32-S3-RS485-CAN-Schematic.pdf):

- Die CAN-Klemme `J5` ist **zweipolig**: nur CANH und CANL. Es gibt dort keine
  eigene Masseklemme.
- Transceiver ist ein **TJA1051T/3**, die Trennung liefert ein
  **B0505LS-1W** DC-DC-Wandler.
- Zwischen der Logikmasse `GND` und der CAN-seitigen Masse `SGND` sitzt aber
  **R31, ein 0-Ω-Widerstand**. Die galvanische Trennung ist also als Option
  vorgesehen und im Auslieferungszustand überbrückt.

Daraus folgt: die Board-Masse *ist* der CAN-Bezugspunkt. Ohne sie fehlt dem
Bus der gemeinsame Bezug, das Gleichtaktpotential kann aus dem zulässigen
Bereich des Transceivers driften, und man sieht auf der Statusseite einen
steigenden TX-Fehlerzähler bei RX = 0 — gesendet, aber niemand quittiert.

> Gegenprobe, falls es damit nicht läuft: Durchgangsprüfer zwischen dem Minus
> der DC-Klemme und **Pin 2 des TJA1051** (dessen GND). Piept es, ist R31
> bestückt und die Board-Masse der richtige Anschlusspunkt. Kein Durchgang
> heißt, die Trennung ist auf diesem Exemplar tatsächlich offen.

Hängt das Board an einem eigenen Netzteil an der DC-Klemme, ist das Anklemmen
unkritisch. Hängt es am USB eines geerdeten Rechners, verbindet man zwei
Erdungspunkte — dann besser vorher auf ein separates Netzteil wechseln.

Den 120-Ω-Jumper nur auf `120R` setzen, wenn das Gerät physisch am Busende
sitzt — sonst auf `NC`. Zwei Abschlusswiderstände zu viel legen den Bus lahm,
und das äußert sich als `bus-off` auf der Statusseite.

E3-Anlagen haben oft mehrere CAN-Stränge (interner Bus zwischen den Modulen,
separater Anlagen-/Zubehörbus). Nur auf einem davon liegt der Verkehr, den
open3e erwartet — die Bus-Diagnose unten sagt, ob am angeschlossenen Strang
überhaupt jemand sendet.

### Läuft der Build, den ich gerade gebaut habe?

Die Statusseite zeigt **Build-Zeitpunkt** und eine **Build-Kennung** — die ersten
Zeichen der ELF-Prüfsumme, die sich bei jedem geänderten Byte Code ändert.
Lokal:

```sh
make fwinfo
```

```
  project        open3e-gateway
  version        0.2.0+ec2f7f9
  built          Sep  2 2026 00:19:24
  ESP-IDF        v6.1
  build identity 068bd904f9f67bcf
```

Stimmt die Kennung mit der auf der Statusseite überein, läuft genau dieser
Build. Das ist verlässlicher als die Versionsangabe: die ist nur so gut wie die
Disziplin, sie hochzuzählen.

Die Version selbst steht in `version.txt` (semantisch, z. B. `0.2.0`) und wird
mit jedem Release zusammen mit `CHANGELOG.md` erhöht. Der Build hängt den
Commit als Metadatum an — `0.2.0+ec2f7f9`, bei einem veränderten
Arbeitsverzeichnis `0.2.0+ec2f7f9-dirty` —, damit zwei Builds derselben
Version unterscheidbar bleiben. `/api/status` liefert beides getrennt:
`version` für Programme, die eine Mindestversion prüfen, `firmware` für
Menschen. Die Raw-API für externe Integrationen hat eine eigene Nummer
(`rawApiVersion`), die nur bei Änderungen an dieser Schnittstelle steigt;
die Statusseite zeigt beide in der Karte „Gerät".

### Auslastung

Auf der Statusseite: CPU-Anteil und Stack-Reserve **pro Task**, dazu Heap
getrennt nach internem RAM und PSRAM.

Die CPU-Anteile werden gegen die vorige Abfrage gemessen, sind also die Last
über das letzte Poll-Intervall statt eines Durchschnitts seit dem Start — ein
Durchschnitt würde eine Task verstecken, die nur während eines Scans entgleist.

Zwei Spalten lohnen den Blick:

- **Interner RAM.** Der ist der knappe. Die 8 MB PSRAM lassen eine Gesamtsumme
  beruhigend aussehen, während genau der Teil ausgeht, auf den Netzwerkstack
  und Interrupt-Handler angewiesen sind.
- **Stack frei.** Der tiefste Punkt, den eine Task je erreicht hat. Geht der
  gegen Null, stürzt sie irgendwann ab — unter 512 Byte wird der Wert rot.

Kostet einen Zählerstand pro Kontextwechsel
(`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`). Auf einem Gerät, dessen Aufgabe ein
250-kBit/s-Bus ist, ist das nicht der Engpass — und es macht eine Task, die im
falschen Kontext blockiert, sichtbar, bevor sie zum Absturz wird.

### Bus-Diagnose

Auf der Statusseite. Zwei Tests, die zusammen sagen, wo es klemmt:

1. **Loopback** — der Controller sendet sich selbst ein Frame, mit
   abgeschalteter Quittung. Besteht er das nicht, ist es Firmware oder
   Pin-Konfiguration und definitiv nicht die Verkabelung.
2. **Listen-Only-Sweep** über 250, 125, 500 und 1000 kBit/s. In diesem Modus
   sendet und quittiert das Gerät nichts, kann einen intakten Bus also nicht
   stören. Ankommende Frames beweisen Verkabelung *und* Bitrate.

| Ergebnis | Bedeutung |
|---|---|
| Loopback fehlgeschlagen | Firmware/Pins |
| Loopback ok, 0 Frames auf allen Bitraten | CAN-Masse fehlt, CAN-H/L vertauscht, falscher Strang, oder niemand sendet |
| Frames bei anderer Bitrate | Bus läuft nicht mit 250 kBit/s |
| Frames bei 250 kBit/s | Empfang ok — steigt TX trotzdem, fehlt die Quittung |

**TEC steigt, REC bleibt 0** ist dabei die aussagekräftigste Einzelbeobachtung:
auf CAN quittiert jeder andere aktive Knoten jedes Frame, also heißt das
„gesendet, niemand da".

Der Empfangs-Interrupt gibt einen angeforderten Task-Wechsel an den
TWAI-Treiber **zurück**, statt ihn selbst auszulösen: ein `portYIELD_FROM_ISR()`
im Callback bricht die Interrupt-Behandlung des Treibers mittendrin ab und
äußert sich als Busfehler, sobald viel Verkehr läuft — etwa während eines Scans.

Auf der Statusseite stehen die Fehlerzähler TEC/RX des Controllers. Die steigen
messbar an, lange bevor der Knoten tatsächlich `bus-off` geht — das ist der
früheste sichtbare Hinweis auf ein Verkabelungs- oder Bitraten-Problem.

Die Karte „CAN-Bus" zeigt vier Zähler, die Verschiedenes bedeuten:

| Zähler | Was er zählt | Verhalten |
|---|---|---|
| Busfehler | jeden vom Controller erkannten Bit-, Stuff-, Form-, CRC- oder ACK-Fehler | läuft seit dem Start auf, Neustart setzt ihn zurück |
| Fehlerzähler TX / RX | TEC und REC des Controllers | springen bei einem Fehler hoch (TX um 8, RX um 1), gehen mit jedem fehlerfreien Frame um 1 zurück; ein kurzer Ausschlag ist normal und meist schon weg, bevor die Seite ihn abholt |
| Frames verpasst | Frames, die der Empfangs-Interrupt nicht mehr in die Warteschlange bekam | die einzigen Frames, die diese Firmware wirklich verworfen hat; sollte 0 sein |
| Bus-Off-Recovery | wie oft der Controller nach `bus-off` neu gestartet wurde | 0 auf einem gesunden Bus |

Ein paar Busfehler pro Minute hat fast jede Anlage; ein paar pro Sekunde sind
ein Abschluss-, Masse- oder Störungsproblem, auch wenn alle Werte ankommen.
Ein **„no response"** oder ein fehlgeschlagener Read ist dagegen kein
Busfehler: die Anfrage wurde gesendet und quittiert, die ECU hat innerhalb der
Frist nicht geantwortet. Die beiden Dinge hängen trotzdem zusammen. Trifft ein
Busfehler genau das Anfrage-Frame, ist die Anfrage zerstört, der Sender muss
sie wiederholen. Der Controller tut das seit 0.3.0 bis zu dreimal in Hardware,
so wie jeder socketcan-Adapter; davor ging die Anfrage in dem Moment verloren,
TEC zuckte kurz um 8 und eine Sekunde später stand „no response" im Log, ohne
dass die ECU je etwas gesehen hatte. Für externe Clients wie ioBroker.e3oncan
wartet `rawread` außerdem 2,5 s statt 1 s auf die Antwort, einstellbar per
`timeout` (siehe `docs/raw-gateway-api.md`), weil die HMU einer beschäftigten
Anlage gelegentlich länger braucht.

**Wichtig:** Es darf immer nur *ein* Master am Bus sein. Läuft parallel noch ein
open3e auf einem Raspberry Pi, stören sich die beiden ISO-TP-Sitzungen
gegenseitig. Zum Vergleichen abwechselnd betreiben, nie gleichzeitig.

---

## Ohne Toolchain: aus dem Browser flashen

<https://esp32can.thomas-peterson.de>

Board per USB anstecken, Knopf drücken. Der Browser schreibt über die
**Web Serial API** direkt in den Flash — keine ESP-IDF, kein Treiber, kein
Raspberry Pi. Das Board meldet sich über die native USB-Serial-JTAG-Schnittstelle
des ESP32-S3, die esptool-js unmittelbar anspricht.

Es braucht **Chrome** oder **Edge** ab 89, **Opera** ab 76, **Firefox** ab 151
oder **Chrome für Android** ab 151. Safari und iOS können es nicht — WebKit
lehnt Web Serial ab; die Seite sagt das, statt einen wirkungslosen Knopf zu
zeigen.

Die Seite liegt in [`site/`](site/). GitHub Actions baut sie bei jedem Push,
der Webserver holt sich das Ergebnis selbst (siehe [`site/README.md`](site/README.md));
die Binärdateien und das Manifest entstehen aus dem Build, damit die
Flash-Offsets nicht von `partitions.csv` abweichen können.

Wer selbst entwickelt, nimmt den Weg darunter.

---

## Bauen und flashen

### Voraussetzungen

ESP-IDF **v6.x** wird gebraucht und ist nicht Teil dieses Repos. v6 ist keine
freie Wahl: die Firmware nutzt die TWAI-Node-API (`esp_twai.h`), und cJSON und
esp-mqtt sind dort keine eingebauten Komponenten mehr, sondern werden über den
Component Manager geholt (siehe `main/idf_component.yml`).

```sh
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh          # in jeder neuen Shell
```

### Datenbank und Web-UI erzeugen

Die open3e-Datenpunktdatenbank wird **nicht** mitgeliefert, sondern beim Bauen
aus einer festgepinnten open3e-Version erzeugt. Ein Update auf eine neuere
open3e-Version ist damit eine Zeile in `tools/fetch_open3e.py`.

```sh
make setup          # Python-venv anlegen, open3e holen
make db             # data/o3edb.bin erzeugen  (~1,6 MB)
```

`make storage` ist nur nötig, wenn man das Ergebnis ohne ESP-IDF ansehen will —
der Firmware-Build ruft `tools/build_fs.py` selbst auf.

`storage_image/` liegt bewusst **nicht** unter `build/` — das Verzeichnis
gehört CMake, und `idf.py set-target` verweigert seinen Fullclean, wenn dort
fremde Dateien liegen.

### Firmware bauen und flashen

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Änderungen an `web/` oder an `data/o3edb.bin` werden vom Build erkannt:
`storage_image/` wird neu zusammengestellt, zu `build/storage.bin` gepackt und
von `idf.py flash` mitgeschrieben. Ein separater `littlefs-flash`-Schritt ist
nicht nötig, und `make storage` von Hand auch nicht.

Für das Web-UI genügt also:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

Nur die **Datenbank** bleibt außen vor: `make db` muss man weiterhin selbst
aufrufen, wenn man auf eine neuere open3e-Version wechseln will. Das braucht
einen Netzwerkzugriff und die Python-Umgebung, was ein normaler Firmware-Build
nicht voraussetzen sollte.

Referenzgrößen eines sauberen Builds: App 1,12 MB (73 % der 4-MB-Partition
frei), DIRAM 121 KB von 342 KB.

---

## Erste Einrichtung

Nach dem Flashen erscheint ein WLAN namens **`Open3E-Setup-XXXX`**.

- Passwort: **`open3e-setup`** (im Einrichtungsdialog änderbar)
- Das Captive Portal öffnet die Seite automatisch; falls nicht:
  `http://192.168.4.1`

Nach dem Speichern startet das Gerät neu und ist unter `http://open3e.local`
erreichbar (bzw. unter dem eingestellten Gerätenamen).

**Wenn das WLAN nicht erreichbar ist** — Router getauscht, Passwort geändert —
spannt das Gerät nach 60 Sekunden den Einrichtungs-Hotspot *zusätzlich* wieder
auf und versucht im Hintergrund weiter, sich zu verbinden. Ein kurzer
Router-Neustart heilt sich damit von selbst, ein falsches Passwort bleibt
trotzdem korrigierbar, ohne das Gehäuse zu öffnen.

---

## Bus-Scan

| Modus | Umfang | Dauer |
|---|---|---|
| Schnellscan | nur die 1564 Datenpunkte der mitgelieferten Datenbank | ca. 1 Min. pro ECU |
| Vollscan | DID 256 bis 4000 | 10–20 Min. pro ECU |

Der Scan sucht ECUs auf den COB-IDs `0x680`–`0x6EF` über DID 256
(`BusIdentification`) und liest danach deren Datenpunkte. Das Zwischenergebnis
wird nach jeder ECU gespeichert, ein Neustart mitten im Vollscan verwirft also
nicht alles.

Findet der Scan keine ECU, stimmt fast immer eines davon nicht: Verkabelung,
Abschlusswiderstand, oder ein zweiter Master am Bus.

### Erkanntes Gerät

Beim ECU-Scan liest die Firmware **DID 256** (`BusIdentification`) und **DID 377**
(`ViessmannIdentificationNumber`) und zeigt daraus:

| Feld | Herkunft |
|---|---|
| Typ (`HPMUMASTER`, `EMCUMASTER`, …) | DID 256, `DeviceProperty` |
| Funktion | DID 256, `DeviceFunction` |
| Bustyp | DID 256, `BusType` |
| Software-/Hardware-Version | DID 256 |
| Seriennummer (VIN) | DID 256 |
| Viessmann-Identnummer | DID 377 |

DID 256 wird über den **echten Codec** dekodiert, nicht über feste Byte-Offsets.
Das ist nicht bloß sauberer: der Datensatz hat eine Längenvariante, und
handgerechnete Offsets würden auf einem Gerät, das die andere zurückliefert,
stillschweigend Unsinn produzieren.

### Gerätename und `{device}`

Jede ECU hat einen **frei wählbaren Namen**, editierbar in der Gerätetabelle.
Er füllt den Platzhalter `{device}` in den MQTT-Topics — exakt wie der
Schlüssel in open3es `devices.json`, dessen `dev_of_addr()` ebenfalls den
Benutzerschlüssel liefert und auf die Hex-Adresse zurückfällt.

Default ist deshalb `0x680`, nicht der Gerätetyp: ein Formatstring mit
`{device}` erzeugt so dieselben Topics wie vorher auf dem Pi. Vergebene Namen
überleben einen erneuten Scan.

### „nicht in DB"

Ein Datenpunkt landet nur dann in der Liste, wenn die ECU auf ihn mit einer
**positiven UDS-Antwort** geantwortet hat. Die Markierung „nicht in DB" heißt
also nicht, dass nichts zurückkam — sie heißt, dass open3e für diesen DID keine
Beschreibung mitliefert. Die Antwortlänge steht daneben als Beleg.

Solche Datenpunkte lassen sich trotzdem lesen und nach MQTT senden; der Wert
ist dann ein Hex-String, wie bei open3es Raw-Modus. **Schreiben ist für sie
gesperrt** — Bytes an eine Heizung zu schicken, deren Bedeutung man nicht kennt,
ist genau der Fehler, den es zu verhindern gilt.

Wer so einen DID identifiziert, kann ihn übrigens
[bei open3e beitragen](https://github.com/open3e/open3e/discussions).

---

## MQTT

Standard-Topic ist `open3e/<Formatstring>`, Default-Formatstring `{didName}` —
identisch zu open3e. Unterstützte Platzhalter:

`{didName}` · `{didNumber}` · `{ecuAddr}` · `{device}` ·
`{ecuAddr:03X}` · `{didNumber:04d}`

Pro Datenpunkt lässt sich das Suffix überschreiben und der Modus wählen:

| Modus | Ergebnis |
|---|---|
| **JSON** | ein Topic, Payload das ganze Objekt |
| **geflacht** | ein Topic je Unterfeld, Payload der nackte Wert |

```
open3e/FlowTemperatureSensor
  → {"Actual": 27.2, "Minimum": 21.0, "Maximum": 31.4, ...}

open3e/FlowTemperatureSensor/Actual   → 27.2
open3e/FlowTemperatureSensor/Minimum  → 21.0
```

`open3e/LWT` trägt retained `online` / `offline`.

### Kommandos

Auf `open3e/cmnd` (einstellbar) wird das Schema von open3es Listener akzeptiert:

```json
{"mode": "read",  "data": [268, 269], "addr": "0x680"}
{"mode": "write", "data": {"396": {"Comfort": 22.0}}, "addr": "0x680"}
```

Antworten von `read` landen auf demselben Topic wie eine planmäßige Abfrage,
Fehler auf `open3e/ERR`. Die Raw-Modi von open3es Listener sind hier bewusst
nicht implementiert — der Sinn dieses Kommandokanals ist die dekodierte
Sicht, und ein Raw-Write ist der einfachste Weg, eine Heizung in einen
ungewollten Zustand zu bringen. Für Software mit eigener Datenpunkt-Datenbank
gibt es dennoch einen expliziten, separaten Rohdaten-Zugang, siehe
[Rohdaten für externe Integrationen](#rohdaten-für-externe-integrationen).

### Home Assistant

Auto-Discovery ist zuschaltbar. Pro Blattwert entsteht eine Entität mit Einheit
und passender `device_class` direkt aus dem open3e-Codec. Beim Abschalten werden
die Discovery-Topics wieder geleert, damit keine verwaisten Entitäten
zurückbleiben.

---

## Aus dem Netz laden (Vitocharge VX3)

Auf einer Anlage aus Vitocal, Vitocharge VX3 und E380 schreibt der
Energiemanager der Vitocal alle rund zehn Sekunden den Sollwert am
Netzanschluss auf null — die gewöhnliche Eigenverbrauchsregelung. Sie läuft
lokal und ist in der App nicht abschaltbar; alle Verwaltungsoptionen
auszuschalten ändert nichts daran.

Der Sollwert selbst lässt sich aber überschreiben:

| | |
|---|---|
| Datenpunkt | DID **2188** `PointOfCommonCouplingSetActivePowerTotal` |
| ECU | die Speichereinheit, dort `EMCUSLAVE` |
| Format | 6 Byte: `int16` Leistung, dann `int32` = 120 |
| Einheit | **Watt**, ohne Skalierung |
| Vorzeichen | **negativ zieht aus dem Netz**, positiv speist ein |

Die Datenbank führt den Punkt als `ro`; das Gerät sieht das anders und quittiert
den Schreibbefehl. Gemessen: `-1000` ergab 1003 W Netzbezug und 1046 W mehr
Ladeleistung, aufgebaut über etwa fünf Sekunden.

Ein einzelner Schreibvorgang hält nur bis zur nächsten Auffrischung des
Reglers. Deshalb schreibt die Firmware alle zwei Sekunden nach.

### Bedienung

Einmalig: *Einstellungen* → **Schreiben freigeben**, und die ECU der
Speichereinheit heraussuchen — im Scan die mit `EMCUSLAVE`.

Von Hand: *Debug* → **Aus dem Netz laden** → Leistung negativ, Dauer in
Minuten, **Halten**.

Aus einer Hausautomatisierung, für den Betrieb an einem dynamischen Tarif,
auf `<base>/cmnd`:

```json
{"mode": "grid", "addr": "0x6A1", "watts": -2000, "seconds": 1800}
{"mode": "grid", "stop": true}
```

Eine Automatisierung, die bei niedrigem Börsenpreis feuert, schickt genau
diese eine Nachricht und erneuert sie, solange das Preisfenster offen ist.

Ob es wirkt, steht in `PointOfCommonCouplingPower` (positiv = Bezug) und
`ElectricalEnergyStorageCurrentPower` (negativ = lädt). Bei einem
funktionierenden Halten bleiben die stabil, statt nach zehn Sekunden
zurückzufallen. Mehr als die Ladegrenze aus DID 1828 nimmt die Anlage nicht
an, gleich was gefordert wird.

Gemessen an einer laufenden Anlage: angefordert −2000 W, am Netzanschluss
2000 W Bezug, Batterie 2400 W Ladeleistung bei 917 W PV und 446 W
Hausverbrauch — die Bilanz geht bis auf 71 W Wandlungsverlust auf.

**Es bleibt nichts zurück.** Zeitablauf, Neustart, Absturz, Stromausfall,
abgezogenes Kabel — jedes davon beendet das Halten, und der Energiemanager hat
den Sollwert binnen zehn Sekunden von selbst zurück. An der Anlage wird nichts
verändert; das Gerät redet nur lauter als sie. Genau deshalb ist es so gebaut
und nicht als dauerhafte Umkonfiguration.

---

## Schreiben auf den Bus

Zwei Sperren, weil am anderen Ende eine echte Heizung hängt:

1. **Global aus.** „Schreiben freigeben" muss in den Einstellungen aktiviert
   werden. Standard ist deaktiviert.
2. **Pro Datenpunkt.** Geschrieben wird nur, was die open3e-Datenbank als `rw`
   führt.

Vor jedem Schreiben liest die Firmware den Datenpunkt zuerst — das bestätigt,
dass er auf dieser ECU existiert, und die Antwortlänge entscheidet, welche
Codec-Variante gilt.

Datenpunkte, die open3e selbst nicht kodieren kann (Text-, MAC-, Datumsfelder:
`not implemented yet`), werden abgelehnt statt mit einer selbst erfundenen
Kodierung geschrieben.

---

## Rohdaten für externe Integrationen

Für Software, die CAN-Frames selbst decodiert statt sich auf den Codec dieser
Firmware zu verlassen — etwa weil sie ihre eigene Datenpunkt-Datenbank
pflegt —, gibt es eine zweite, von der dekodierten Sicht oben komplett
unabhängige Schnittstelle auf Rohbyte-Ebene:

- **REST:** `GET /api/rawread` (bis zu 10 DIDs je Aufruf, Wartezeit je DID
  per `timeout=<ms>` wählbar, sonst 2,5 s) liefert die rohen
  UDS-Antwortbytes, `POST /api/rawwrite` (Service `0x2E`) schreibt sie.
- **MQTT:** die in der Einstellung `rawCanIds` (Komma-Liste) eingetragenen
  CAN-IDs werden roh auf `<baseTopic>/raw/<id-hex>` veröffentlicht —
  unabhängig von `points.json` und Auto-Discovery.

Beide Wege laufen komplett an der open3e-Datenbank vorbei — nichts davon wird
dekodiert oder auf `rw` geprüft. Vollständige Beschreibung, inklusive der
Scan-Delegation für einen entfernten Client:
[`docs/raw-gateway-api.md`](docs/raw-gateway-api.md) — entstanden für die
Integration mit [ioBroker.e3oncan](https://github.com/MyHomeMyData/ioBroker.e3oncan),
der ersten Software, die diesen Weg nutzt.

---

## Wie das intern zusammenhängt

```
TWAI (GPIO15/16, 250 kBit/s, esp_twai Node-API, Empfang per ISR-Callback)
  └── isotp.c      ISO 15765-2, STmin 10 ms, BS 0, Padding 0x00
       └── uds.c   ISO 14229: 0x22 lesen, 0x2E schreiben
            └── can_port.c   eine Task besitzt den Bus, alle anderen stellen
                             Requests in eine Queue
                 ├── e3_scan.c   ECU- und DID-Discovery
                 ├── poller.c    zyklische Abfrage der Auswahl
                 └── httpd_api.c Web-UI und REST
```

**Warum nur eine Task den Bus anfasst:** open3e warnt „do not start more than
one instance". Hier wären die Konkurrenten intern — Poller, Scanner, Web-UI,
MQTT-Kommandos. Alle laufen über eine Queue, damit überlappende ISO-TP-Sitzungen
strukturell unmöglich sind statt nur unwahrscheinlich.

**Warum der Codec zweistufig ist:** `o3e_codec_compile()` baut aus der
JSON-Beschreibung einmalig einen kompakten Baum, wenn ein Datenpunkt ausgewählt
wird. Das Abfragen läuft danach nur noch auf diesem Baum — kein Dateizugriff,
kein JSON-Parser. Deshalb sind Sekundenintervalle über Dutzende Datenpunkte
bezahlbar.

**Speicheraufteilung:** Die kleinen, heißen Tabellen (Datenpunkt-Index, Namen,
Enum-Tabellen, zusammen ~145 KiB) liegen im PSRAM. Die ~1,4 MB
Codec-Beschreibungen bleiben im Flash und werden einzeln gelesen.

---

## Tests

Der C-Port wird nicht nach Augenmaß bewertet, sondern gegen open3e gemessen:
Die Testvektoren entstehen, indem die **echte Python-Implementierung** läuft.

```sh
make setup      # einmalig
make fixtures   # Vektoren aus open3e erzeugen
make test
```

| Test | Prüft | Umfang |
|---|---|---|
| `test_codec` | Dekodieren == open3e | 4692 Vektoren über alle 1564 DIDs |
| `test_encode` | Kodieren == open3e, **und** dieselben Ablehnungen | 3990 + 332 |
| `test_flatten` | geflachtes MQTT == open3es `mqttdump()` | 23 395 Topic/Wert-Paare |
| `test_isotp` | Segmentierung, Flow Control, SN-Wrap, Frame-Verlust | bis 4095 Byte |
| `test_uds` | Antwort, Schweigen, negative Antwort, responsePending | 5 Szenarien |
| `test_em380` | E380-Dekodierung == E3onCAN | 560 Vektoren über 14 CAN-IDs |
| `test_contact` | Entprellung an Gleich- und Wechselspannung, Topic-Namen | 6 Signalformen |
| `test_knx` | KNXnet/IP-Telegramme byteweise gegen die Spezifikation | 8 Frame-Arten, 14 Adressfälle |
| `test_topic` | Topic-Expansion der Weboberfläche == `mqtt_pub_topic()` | 18 Fälle, 3 an der Anlage geprüft |

`make lint` prüft zusätzlich Includes und modulübergreifende Symbole der
Firmware-Quellen, die sich ohne ESP-IDF nicht übersetzen lassen.

`test_uds` sichert eine Eigenschaft, die einmal gebrochen war: **eine Anfrage,
die nicht oder negativ beantwortet wird, darf keinen Erfolg melden.** Requests
liefen früher über eine FreeRTOS-Queue an eine CAN-Task; `xQueueSend` kopiert
per Wert, also füllte die Task das Ergebnis in ihrer eigenen Kopie aus, während
jeder Aufrufer seine eigene, null-initialisierte zurücklas — und ein genullter
`uds_result_t` liest sich als `UDS_OK`. Jede Anfrage schien zu gelingen, der
Bus-Scan „fand" an jeder Adresse ein Gerät und hinter fast jedem DID einen
Datenpunkt. Die Task und die Queue sind ersatzlos entfallen: der Austausch
läuft jetzt im aufrufenden Task unter einem Mutex, das Ergebnis ist der
Rückgabewert.

Zwei Details, die beim Portieren leicht schiefgehen und deshalb explizit
abgedeckt sind: `scale` ist beim **Dekodieren ein Divisor** (`O3EInt16` hat
Default 10.0), und vorzeichenlose 64-Bit-Zähler dürfen nicht über `int64_t`
laufen. Beide Fehler waren im ersten Wurf drin und sind von den Vektoren
gefunden worden.

### Test gegen einen simulierten E3

open3e testet selbst gegen
[virtualE3](https://github.com/philippoo66/vitualE3). Gleicher Aufbau, nur mit
echtem CAN statt `vcan0`:

```
virtualE3 (Docker, -c can0) ── USB-CAN-Adapter @250k ── CAN ── ESP32-S3
```

Erwartung: Die Firmware findet die ECUs `0x680` und `0x6a1` und dekodiert deren
Datenpunkte identisch zu open3e.

---

---

## Energiezähler E380 (passiv)

Der Viessmann **E380** beantwortet keine UDS-Anfrage — er sendet unaufgefordert
acht Byte auf den CAN-IDs `0x250`–`0x25D`, und das ist das gesamte Protokoll.
open3e sieht ihn deshalb gar nicht. Die Unterstützung ist aus
[E3onCAN](https://github.com/MyHomeMyData/E3onCAN) portiert.

- **Standardmäßig aus.** Unter *Einstellungen → System* einschalten. Ohne
  Zähler bringt es nichts und liefe nur im Empfangs-Interrupt mit.
- **Rein passiv.** Es wird nichts auf den Bus geschrieben, nur ein
  Empfangsfilter gesetzt.
- Gerade CAN-IDs gehören zum Zähler mit CAN-Adresse **97**, ungerade zu **98**.
- Frames, die nie ankommen, erscheinen im Reiter *Smart Meter* ohne Wert — so
  ist „kein Zähler am Bus" von „Zähler da" unterscheidbar.

| CAN-ID | Inhalt |
|---|---|
| 0x250/0x251 | Wirkleistung L1–L3 und Summe |
| 0x252/0x253 | Blindleistung |
| 0x254/0x255 | Ströme und cosPhi |
| 0x256/0x257 | Spannungen und Frequenz |
| 0x258/0x259 | Bezug/Einspeisung kumuliert (Float32) |
| 0x25A/0x25B | Wirk- und Blindleistung (32 Bit) |
| 0x25C/0x25D | Bezug kumuliert (32 Bit) |

Das **Intervall** ist hier eine Untergrenze, kein Zeitplan: der Zähler sendet
mehrmals pro Sekunde, gesendet wird aber nur, was auch empfangen wurde.

Die Decodier-Task läuft mit niedrigerer Priorität als Poller und Scan — das
Dekodieren eines Broadcasts darf einen ISO-TP-Austausch nicht verzögern, dessen
Frame-Timing die engere Vorgabe ist. Die kompilierten Codec-Bäume sind durch
eine Mutex geschützt: sie werden bei jedem Speichern der Auswahl neu gebaut,
auch am Ende eines Scans, und das gibt genau die Bäume frei, die die
Decodier-Task in dem Moment durchläuft.

Zwei Codec-Details weichen von open3e ab und sind deshalb einzeln gegen
E3onCAN gemessen (`test_em380`): dessen `O3EFloat32` teilt durch einen
`scale`-Faktor, und `O3EcosPhi` nimmt den Betrag aus dem zweiten Byte und das
Vorzeichen aus dem ersten.

Das **Collect-Protokoll** für die E3-Geräte selbst (Vitocal, VX3, …) ist
portiert (`collect.c`), und es lohnt sich zu wissen, was es wirklich ist: kein
eigenes Format, sondern gewöhnliches ISO-TP mit UDS-Service `0x77`. Der
Nutzinhalt sieht so aus:

    77 <Zähler lo> <Zähler hi> <Kennung> 01 82 <Block> <Block> …
    Block := <DID lo> <DID hi> <Typ> <Daten>

Der sechs Byte lange Kopf endet genau dort, wo der erste Folgeframe beginnt.
Deshalb landet ein Parser, der sich an den `0x21`-Frame hängt, unmittelbar auf
`<DID> <Typ> <Daten>` und bekommt das richtige Ergebnis, ohne irgendetwas
zusammenzusetzen. Dass das ein Zufall ist, sollte man wissen.

Der Typ nennt die Länge: `0xB1`–`0xB4` passen neben den Kopf, `0xB5`–`0xBF`
laufen in den nächsten Frame über, `0xB0` heißt, die Länge folgt (ein Byte
später, wenn dieses Byte `0xC1` ist). Fehlt einer dieser Zweige, verschwinden
die betroffenen Nachrichten *lautlos* — kein Fehler, keine Zählung. Genau das
prüft `test_collect` gegen die Rahmung eines echten Busses.

---

## Kontakteingänge und Schaltausgänge (Klingel, Türkontakt, SG-Ready-Relais)

Der SH1.0-Stecker des Boards führt **GND, 3V3, GPIO1 und GPIO2** heraus. Beide
Pins sind sonst unbenutzt: der CAN-Transceiver hängt an 15/16, der
RS485-Treiber an 17/18/21, und Strapping-Pins sind auf dem ESP32-S3 nur 0, 3,
45 und 46. Damit kann das Gerät, das ohnehin im Heizungsraum hängt, nebenbei
zwei Schalter melden — eine Klingel, einen Türkontakt, einen
Schwimmerschalter, einen Störmeldekontakt — oder, je Pin wählbar, zwei
Relais schalten.

Einschalten unter *Einstellungen → Gerät → Kontakteingänge und
Schaltausgänge*, Betriebsart je Pin *Eingang* oder *Ausgang*. Jeder aktive Eingang

- sendet retained nach `<Basis>/contact/<name>` den Wert `ON` oder `OFF`,
- erscheint per Auto-Discovery als **binärer Sensor** in Home Assistant,
- und zeigt seinen Zustand samt Auslösezähler direkt in der Weboberfläche.

Der Name bestimmt das Topic: „Klingel Haustür" wird zu
`open3e/contact/klingel_haustuer`. Umbenennen verschiebt also das Topic. Ohne
Namen heißt der Eingang schlicht **Eingang 1** bzw. **Eingang 2** — die Pins
sind generisch, der Standardname ist es auch.

### Schaltausgänge

Die Steuerung liest ihre SG-Ready-Klemmen nur als Kontakte; einen Datenpunkt,
der sie setzt, kennt open3e nicht (DID 543 meldet den Zustand, 2544/2545/2560
sind die Konfiguration, alle nur lesbar). Wer die Wärmepumpe aus einer
Automation heraus sperren oder freigeben will, braucht also ein Relais — und
das Gerät neben den Klemmen kann es schalten.

Ein Pin in der Betriebsart *Ausgang*

- meldet seinen Zustand retained nach `<Basis>/output/<name>` als `ON` oder
  `OFF`,
- nimmt Befehle auf `<Basis>/output/<name>/set` entgegen (`ON`/`OFF`, auch
  `1`/`0` und `true`/`false`, Groß- und Kleinschreibung egal),
- erscheint per Auto-Discovery als **Schalter** in Home Assistant,
- lässt sich in der Weboberfläche direkt schalten und per `POST /api/output`
  mit `{"gpio": 1, "on": true}`,
- und ist über den Befehlskanal erreichbar: `{"mode": "output", "gpio": 1,
  "on": true}`.

Eine eingetragene KNX-Adresse bekommt bei jedem Schalten den neuen Zustand.
*Ein bedeutet* legt fest, ob der Pin im eingeschalteten Zustand auf 3V3 oder
auf GND liegt — die meisten günstigen Relaismodule schalten bei Low. **Nach
jedem Neustart ist ein Ausgang aus**; eine Sperre, die einen vergessenen
Neustart überlebt, wäre schlimmer als eine, die abfällt. Beim Speichern der
Einstellungen bleibt der Zustand dagegen erhalten.

### Verkabelung

> **An den Pins dürfen höchstens 3,3 V anliegen.** Sie sind nicht geschützt.
> 5 V zerstört sie, 12 V zerstört mehr als sie. Und sie liefern nur wenige
> Milliampere: ein Relaismodul braucht eine Transistorstufe, eine nackte
> Relaisspule lässt sich nicht direkt treiben.

**Relaismodul** am Ausgang — ein übliches 1- oder 2-Kanal-Modul mit
Optokoppler oder Transistor, Versorgung 3,3 V oder 5 V je nach Modul:

```
GPIO1 ── IN1 des Moduls        (Signal, 3,3 V genügt bei fast allen Modulen)
GND   ── GND des Moduls
3V3   ── VCC des Moduls        (bei 5-V-Modulen: VCC an 5 V vom USB-Anschluss)
Relaiskontakt (COM/NO) ── SG-Ready-Klemmen der Wärmepumpe
```

Schaltet das Modul bei Low, in den Einstellungen *Ein bedeutet: Pin auf GND*
wählen; sonst zieht das Relais an, solange der Ausgang aus ist.

**Potentialfreier Kontakt** — Taster, Reedkontakt, Relaiskontakt. Direkt
anklemmen, ohne Bauteile:

```
GPIO1 ──┬── Kontakt ── GND
        │
     (interner Pull-up, in der Firmware gesetzt)
```

In den Einstellungen *Kontakt gegen GND (Pull-up)* wählen — die Voreinstellung.

**Eine echte Klingel** ist kein potentialfreier Kontakt. Deutsche
Klingelanlagen laufen mit **8–12 V Wechselspannung**; direkt angeklemmt ist
der ESP32 hinüber. Dazwischen gehört ein Optokoppler:

```
Klingel-Trafo ── Taster ──┬── Klingelspule ──┐
                          │                  │
                          └─ 2,2 kΩ / 1 W ── LED des PC814 ──┘

PC814, Transistorseite:   Kollektor ── GPIO1
                          Emitter   ── GND
```

Der PC814 hat zwei antiparallele LEDs und verträgt Wechselspannung direkt;
mit einem PC817 braucht es zusätzlich einen Brückengleichrichter oder eine
antiparallele Diode. Der Vorwiderstand ist für 12 V~ gerechnet (≈ 7 mA
Effektivstrom); bei 8 V~ tut es auch 1,5 kΩ. Genauso gut: ein
Klingeltrafo-Relais mit potentialfreiem Kontakt — dann gilt wieder der obere
Fall.

Ist am Klingeltrafo nichts zu ändern, gibt es den Abgriff auch fertig als
„Klingel-WLAN-Modul" mit Relaisausgang.

### Warum es eine Abfallzeit gibt

Ein Optokoppler an Wechselspannung liefert **kein Level, sondern ein
Rechteck**: eine Halbwelle leitet, die nächste nicht, 50 Hz lang, solange
jemand auf den Knopf drückt. Eine normale Entprellung — „N gleiche Abtastungen
hintereinander" — kommt darauf nie zur Ruhe und meldet aus einem Klingeln
hundert.

Deshalb ist die Entprellung unsymmetrisch: **zwei** aufeinanderfolgende
aktive Abtastungen (10 ms) machen den Eingang aktiv, und erst eine
ununterbrochene Ruhe von **Abfallzeit** Millisekunden macht ihn wieder
inaktiv. 150 ms überbrücken die 10-ms-Lücken eines zerhackten Signals
bequem und entprellen zugleich jeden Taster. Wird ein Klingeln trotzdem als
mehrere gezählt, höher stellen.

Getestet wird das nicht an der Haustür, sondern in `test_contact` gegen ein
synthetisiertes Signal — sauberer Druck, prellender Druck, zerhackte
Wechselspannung, einzelne eingestreute Spitze, Netzaussetzer mitten im
Klingeln. Ohne die unsymmetrische Regel meldet der Test 100 Klingelzeichen in
zwei Sekunden statt einem.

## KNX

Ein Kontakteingang kann beim Schalten zusätzlich ein **1-Bit-Telegramm
(DPT 1.001)** auf eine KNX-Gruppenadresse schreiben: geschlossen sendet 1,
offen sendet 0. Gedacht für die Klingel — so funktioniert sie auch dann, wenn
Broker oder Home Assistant gerade stehen.

Einschalten unter *Einstellungen → KNX*, die Gruppenadresse steht oben beim
jeweiligen Eingang. Beide Eingänge dürfen verschiedene Adressen bedienen.

Es wird ausschließlich **geschrieben**. Nichts wird gelesen, nichts abonniert,
und **KNX Secure wird nicht unterstützt** — eine gesicherte Installation
ignoriert, was hier gesendet wird.

### Tunnelling oder Routing

KNXnet/IP ist in beiden Fällen UDP; TCP käme erst mit KNX IP Secure.

| | Tunnelling | Routing |
|---|---|---|
| Ziel | ein IP-Interface, per Unicast | `224.0.23.12:3671`, Multicast |
| Zustand | Verbindung, Kanal-ID, Sequenznummern, ACK je Telegramm, Heartbeat alle 60 s | keiner |
| Voraussetzung | jedes IP-Interface kann es | ein IP-**Router**, der wirklich weiterleitet |
| Eigene Adresse | vom Gateway zugewiesen | selbst eintragen, z. B. `1.1.250` |

**Routing passt besser zu einer Klingel** — ein Ereignis am Tag, und keine
Verbindung, die dafür offengehalten werden müsste. Es setzt aber voraus, dass
ein Gerät Routing-Telegramme tatsächlich auf die Zweidrahtleitung legt. Ein
IP-*Interface* tut das meist nicht, **auch wenn es die Dienstfamilie Routing in
seiner Suchantwort meldet**. Ob es geht, sieht man am schnellsten so:

```sh
# 30 s auf der Routing-Gruppe mithören, während jemand einen Taster drückt
python3 - <<'EOF'
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 3671))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
             socket.inet_aton("224.0.23.12") + socket.inet_aton("0.0.0.0"))
while True:
    d, a = s.recvfrom(1024)
    if struct.unpack("!H", d[2:4])[0] == 0x0530:
        print(a[0], d.hex())
EOF
```

Kommt dabei nichts an, bleibt Tunnelling. Dessen Haken: ein Interface hat nur
eine Handvoll Tunnel-Kanäle, und ETS oder eine Visualisierung belegen sie —
das Gerät meldet dann `no free tunnel channel` im Zustand unter den
Einstellungen.

### Warum der Telegrammaufbau einen eigenen Test hat

Ein Telegramm mit falschem Längenbyte wird vom Gateway **kommentarlos
verworfen**: kein Fehler, kein Protokolleintrag, nichts auf dem Bus. Es gibt
nichts zu beobachten und nichts einzugrenzen. Deshalb liegt der ganze
Byte-Aufbau in `knx_frame.c` ohne Socket daneben, und `test_knx` prüft ihn
gegen die Layouts aus KNX 3/8/2 (Core), 3/8/4 (Tunnelling) und 3/6/3 (cEMI) —
Byte für Byte, einschließlich der Längenoktette und der Ablehnung von Frames,
die eine größere Länge behaupten als das Datagramm hat.

Das häufigste Missverständnis steckt im NPDU-Längenbyte: ein 1-Bit-Wert reitet
**im APCI-Oktett selbst** mit, die Länge ist also 1 und nicht 2. Setzt man dort
eine 2, meldet der Test vier fehlgeschlagene Vergleiche; auf dem Gerät hätte
man ein stummes Telegramm.

## evcc

evcc liest die Werte dieses Gateways über sein **MQTT-Plugin** und kann den
Speicher damit auch **steuern**. Unter *Einstellungen → evcc* erzeugt das Gerät
den fertigen Konfigurationsblock — aus der tatsächlichen Datenpunkt-Auswahl,
nicht aus einer Vorlage. Topics, Feldnamen und Zeitfenster stimmen also mit
dem überein, was die Firmware wirklich veröffentlicht.

| evcc | DID | Datenpunkt | Feld |
|---|---|---|---|
| Netz | 1603 | `PointOfCommonCouplingPower` | `ActivePower` |
| PV | 1690 | `ElectricalEnergySystemPhotovoltaicStatus` | `ActivePower cumulated` |
| Speicherleistung | 1836 | `ElectricalEnergyStorageCurrentPower` | — |
| Ladestand | 1664 | `ElectricalEnergyStorageStateOfCharge` | — |

evccs Vorzeichen: positiv ist einströmend — Netzbezug, PV-Erzeugung,
Speicher**entladung**. Die VX3 hält sich daran (DID 1836 beschreibt sich selbst
als „Battery Discharging (positive values)"), deshalb steht nirgends ein
`scale: -1`. Für PV ist 1690 die AC-Summe über die Strings; 1831 wären die
DC-Eingänge je String, die man addieren müsste und die um die
Wechselrichterverluste zu hoch lägen.

### Steuerung

`batterymode` ist ein Setter für Custom-Batteriemeter. evcc ruft ihn mit einer
Zahl auf, die die erzeugte Konfiguration auf die Befehle dieses Gateways
abbildet:

| evcc | bedeutet | Nutzlast |
|---|---|---|
| 1 `normal` | Hold beenden | `{"mode":"grid","addr":"0x6A1","stop":true}` |
| 2 `hold` | nicht entladen, PV-Laden erlaubt | `{"mode":"storage","addr":"0x6A1","storage":"nur laden","seconds":3600}` |
| 3 `charge` | aus dem Netz laden | `{"mode":"grid","addr":"0x6A1","watts":-2000,"seconds":3600}` |

Zwei Dinge daran sind nicht verhandelbar, beide an einer laufenden Anlage
nachgemessen:

**Die Adresse muss in der Nutzlast stehen.** Ohne `addr` geht der Befehl an die
Vorgabe `0x680`, und die trägt auf einer Anlage mit Wärmepumpe keinen der
beiden Datenpunkte — die liegen dort auf `0x6A1` (EMCUSLAVE). Der Hold meldet
sich dann als *aktiv*, schreibt nie, und ist nach fünf Fehlversuchen weg:

```
ohne addr:   active=True  writes=0  failures=3 → 5, dann tot
mit addr:    active=True  writes=2 → 4 → 6, failures=0
```

**Der `watchdog` ist nicht optional.** evcc schickt `batteryMode` nur bei
Änderung, und ein Hold endet hier nach höchstens einer Stunde von selbst — das
ist die Sicherheitseigenschaft, nicht ein Versehen. Der Watchdog schreibt alle
zehn Minuten nach; verschwindet evcc, greift die Frist trotzdem.

### Warum kein EEBUS

Die Frage kommt naheliegenderweise auf. Vier Gründe, alle geprüft:

1. **Über EEBUS kann evcc den Speicher nicht steuern.** Dessen
   EEBUS-Meter sind „EEBus grid meter" (Anwendungsfall MGCP, nur Netzzähler)
   und „EEBus consumer meter". Kein Batteriemeter, kein Ladestand, keine
   Steuerung.
2. **Über MQTT kann es das** — siehe oben.
3. **Von der Konfiguration nähme EEBUS nur den Netzzähler ab.** PV, Speicher,
   Ladestand und Steuerung blieben Handarbeit.
4. **Die Kosten träfen die knappste Ressource.** SHIP (mDNS, Selbstzertifikat
   mit SKI, TLS mit gegenseitiger Authentifizierung, Handshake) plus SPINE
   (Datenmodell, Discovery, Bindings) sind einige tausend Zeilen. Auf diesem
   Board sind im Tiefstand 70 KiB internes RAM frei, größter freier Block
   56 KiB; eine TLS-Verbindung mit Client-Zertifikatsprüfung braucht 30–45 KiB,
   und `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` zwingt mbedTLS' kleine
   Allokationen genau dorthin.

## Sichern und wiederherstellen

Unter *Einstellungen → Sichern und wiederherstellen*. Die Sicherung ist eine
einzelne JSON-Datei mit allem, was ein Gerät in denselben Zustand bringt:

| Enthalten | |
|---|---|
| WLAN | SSID, Passwort, Gerätename, Hotspot-Passwort |
| MQTT | Broker, Port, Zugangsdaten, Topics, Formatstring, HA-Discovery |
| System | Schreibfreigabe, E380, Zeitzone |
| Auswahl | Datenpunkte und Meter-Frames |
| Scan-Ergebnis | optional, abwählbar — der große, aber teuer zu wiederholende Teil |

**Die Datei enthält Passwörter.** Ohne sie wäre das Gerät nach dem
Wiederherstellen nicht erreichbar, eine Sicherung ohne WLAN-Passwort also
weitgehend nutzlos. Entsprechend aufbewahren.

Export und Import gehen beide streamend über die Dateien, ohne das Ergebnis im
Speicher zusammenzubauen — das Scan-Ergebnis allein kann mehrere hundert
Kilobyte groß sein. Nach dem Einspielen startet das Gerät neu.

## Zurücksetzen

Unter *Einstellungen → Zurücksetzen*, je ein Knopf pro Sache:

| Knopf | Löscht |
|---|---|
| Datenpunkt-Auswahl | `points.json` |
| Scan-Ergebnis und Auswahl | zusätzlich `system.json` |
| MQTT-Einstellungen | Broker, Topics, HA-Discovery |
| Alles außer WLAN | alle vier, dann Neustart |
| WLAN vergessen | nur die Zugangsdaten, dann Setup-Hotspot |

Die WLAN-Daten bleiben bei allen außer dem letzten erhalten — ein
Zurücksetzen darf das Gerät nicht unerreichbar machen. Vor dem Löschen einer
Auswahl werden die Home-Assistant-Discovery-Topics geräumt, sonst blieben dort
Entitäten ohne Datenquelle stehen.

---

## Firmware-Update über die Weboberfläche

Unter *Debug → Firmware-Update* lässt sich `build/open3e-gateway.bin` hochladen.

Das Gerät hängt im Heizungsraum, deshalb zwei Absicherungen:

1. **Die laufende Firmware wird nicht angefasst.** Das Abbild geht in die
   zweite App-Partition; erst wenn es vollständig geschrieben und geprüft ist,
   wird die Boot-Partition umgeschaltet.
2. **Rollback.** Eine frisch eingespielte Firmware läuft *auf Probe*. Bestätigt
   sie sich nicht binnen 60 Sekunden als lauffähig, startet der Bootloader beim
   nächsten Reset wieder die vorherige. Der Statusbereich zeigt an, solange der
   Probelauf offen ist.

Vor dem ersten Byte wird geprüft, ob die Datei überhaupt ein ESP32-Abbild ist
(Magic `0xE9`) — eine falsch ausgewählte Datei kann die Reservepartition also
nicht halb beschreiben.

### Weboberfläche und Datenbank

Die liegen auf der Storage-Partition, nicht in der Firmware — und lassen sich
ebenfalls über das Web-UI erneuern:

| Weg | Wofür | Umfang |
|---|---|---|
| *Weboberfläche aktualisieren* | einzelne Dateien aus `storage_image/www/` | wenige KiB |
| *Datenbank aktualisieren* | ganze Partition (`build/storage.bin`) | 4 MB, Neustart |

Der Datei-Weg ist der Alltagsfall, solange sich die Oberfläche noch täglich
ändert: `app.js.gz` hochladen, Seite mit Strg+F5 neu laden, fertig.

> **Nur die Oberfläche zu aktualisieren reicht nicht, wenn eine Funktion neu
> ist.** Dann zeigt die Seite Bedienelemente, die die laufende Firmware nicht
> kennt: eingetragene Werte verschwinden beim Speichern, und die Oberfläche
> meldete trotzdem Erfolg. Genau das ist einmal passiert. Die Seite gleicht
> deshalb jeden Abschnitt gegen das ab, was `/api/settings` wirklich liefert,
> sperrt die Felder und schreibt hin, dass die Firmware zu alt ist. Das
Partitionsabbild braucht man nur, wenn sich die Datenpunktdatenbank geändert
hat — es **löscht dabei Auswahl und Scan-Ergebnis**, weil die auf derselben
Partition liegen. WLAN und MQTT überleben, die stehen im NVS.

Ändert sich das Containerformat der Datenbank, verweigert `o3e_db_open()` den
Dienst und protokolliert das, statt eine alte Datei falsch zu interpretieren.

---

## Was (noch) nicht drin ist

- **RS485.** Hardware vorhanden, keine Anforderung.
- **DoIP** als Alternative zu CAN.
- **Schreiben über UDS-Service 0x77.** Gelesen und dekodiert wird er
  vollständig — auf einem Vitocharge-Bus läuft der gesamte Verkehr zwischen
  Backend-Gateway und Speicher darüber. Selbst schreiben kann die Firmware ihn
  nicht; das ginge nur gegen einen Gateway, der dieselben Datenpunkte alle zehn
  Sekunden neu setzt.

---

## Lizenz

**Apache License 2.0** — siehe [`LICENSE`](LICENSE).

Dieselbe Lizenz wie die beiden Quellen, aus denen dieses Projekt schöpft:
[open3e](https://github.com/open3e/open3e) und
[E3onCAN](https://github.com/MyHomeMyData/E3onCAN) stehen beide unter
Apache-2.0. Der Code in `main/` ist ein eigenständiger Port;
Datenpunktdefinitionen, Enumerationen und Codec-Semantik stammen von dort.
Wer das Projekt weitergibt — auch als fertige Firmware —, gibt
[`NOTICE`](NOTICE) mit: die Datei beschreibt, was woher kommt, und Apache-2.0
verlangt das ausdrücklich.
