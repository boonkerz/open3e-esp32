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

Modul ESP32-S3-WROOM-1, 16 MB Flash, 8 MB PSRAM. Bus mit **250 kBit/s**.

**Verkabelung.** CAN-H und CAN-L an den E3-Bus — und die Busmasse an **GND des
Boards**.

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
  version        20260901-2218
  built          Sep  2 2026 00:19:24
  ESP-IDF        v6.1
  build identity 068bd904f9f67bcf
```

Stimmt die Kennung mit der auf der Statusseite überein, läuft genau dieser
Build. Das ist verlässlicher als die Versionsangabe: die ist nur so gut wie die
Disziplin, sie hochzuzählen — ESP-IDF setzt ohne Git-Tags stumm eine `1` ein,
was nach einem OTA aussieht wie „nichts passiert". Der Buildprozess setzt sie
deshalb aus `git describe`, ersatzweise aus dem Datum.

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

Die Seite liegt in [`site/`](site/) und wird mit `make site` gebaut und mit
`make deploy` ausgerollt; die Binärdateien und das Manifest entstehen aus dem
Build, damit die Flash-Offsets nicht von `partitions.csv` abweichen können.

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

- **REST:** `GET /api/rawread` (bis zu 10 DIDs je Aufruf) liefert die rohen
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
| `test_encode` | Kodieren == open3e, **und** dieselben Ablehnungen | 4013 + 315 |
| `test_flatten` | geflachtes MQTT == open3es `mqttdump()` | 23 100 Topic/Wert-Paare |
| `test_isotp` | Segmentierung, Flow Control, SN-Wrap, Frame-Verlust | bis 4095 Byte |
| `test_uds` | Antwort, Schweigen, negative Antwort, responsePending | 5 Szenarien |
| `test_em380` | E380-Dekodierung == E3onCAN | 560 Vektoren über 14 CAN-IDs |

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
ändert: `app.js.gz` hochladen, Seite mit Strg+F5 neu laden, fertig. Das
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

Der Code in `main/` ist ein eigenständiger Port. Datenpunktdefinitionen,
Enumerationen und Codec-Semantik stammen aus open3e (Apache-2.0) — siehe
[`NOTICE`](NOTICE).
