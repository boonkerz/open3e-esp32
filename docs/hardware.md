# Hardware

| Funktion | GPIO |
|---|---|
| CAN TX (TWAI) | 15 |
| CAN RX (TWAI) | 16 |
| RS485 TX / RX / DE | 17 / 18 / 21 (von dieser Firmware nicht genutzt) |
| Kontakteingang 1 / 2 | 1 / 2 (SH1.0-Stecker, siehe unten) |

Modul ESP32-S3-WROOM-1, 16 MB Flash, 8 MB PSRAM. Bus mit **250 kBit/s**.

**Verkabelung.** CAN-H und CAN-L an den E3-Bus, sonst nichts: die CAN-Klemme
hat keinen Massekontakt, und der CAN-Teil des Boards ist galvanisch getrennt.
Wie das in einer Unterverteilung neben der Heizung aussieht, mit Fotos,
Schaltbild und einem bezahlbaren Stecker für die gelbe Buchse 91:
[`docs/aufbau-praxis.md`](aufbau-praxis.md).

Aus dem [Schaltplan](https://files.waveshare.com/wiki/ESP32-S3-RS485-CAN/ESP32-S3-RS485-CAN-Schematic.pdf):

- Die CAN-Klemme `J5` ist **zweipolig**: nur CANH und CANL. Es gibt dort keine
  eigene Masseklemme.
- Transceiver ist ein **TJA1051T/3**, die Signale laufen über den
  Digital-Isolator **π163E31**, die Versorgung der getrennten Seite liefert
  ein **B0505LS-1W** DC-DC-Wandler.
- **R31, ein 0-Ω-Widerstand**, verbindet die CAN-seitige Masse `SGND` mit
  `EARTH`, dem Ableitpunkt der RS485-Schutzdioden. Mit der Logikmasse `GND`
  hat er nichts zu tun. Die galvanische Trennung ist also wirksam, die
  CAN-Seite hat keine Masseklemme und keinen Bezug zur Board-Masse.

Daraus folgt: Eine Busmasse an GND oder am Minus der DC-Klemme erreicht den
Transceiver nicht, sie ist für den CAN-Teil wirkungslos. Das deckt sich mit
der Viessmann-Anleitung, am Stecker 91 nur CAN-H und CAN-L zu belegen.

> Frühere Fassungen dieser README behaupteten, R31 überbrücke die Trennung und
> die Board-Masse sei der CAN-Bezugspunkt. Das war eine Fehllesung des
> Schaltplans, korrigiert nach Issue #14. Ob der fehlende Massebezug der
> isolierten Seite zu den Busfehlern beiträgt, die Nutzer dieses Boards sehen,
> wird in ioBroker.e3oncan #262 untersucht; die Bus-Diagnose zählt dafür die
> Fehler beim reinen Mithören.

Den 120-Ω-Jumper nur auf `120R` setzen, wenn das Gerät physisch am Busende
sitzt — sonst auf `NC`. Zwei Abschlusswiderstände zu viel legen den Bus lahm,
und das äußert sich als `bus-off` auf der Statusseite.

E3-Anlagen haben oft mehrere CAN-Stränge (interner Bus zwischen den Modulen,
separater Anlagen-/Zubehörbus). Nur auf einem davon liegt der Verkehr, den
open3e erwartet — die Bus-Diagnose unten sagt, ob am angeschlossenen Strang
überhaupt jemand sendet.

> **Vor Inbetriebnahme und Software-Update der Anlage das Gateway vom Bus
> nehmen.** CAN-Stecker abziehen oder das Gerät stromlos machen und erst
> wieder anschließen, wenn die Anlage normal läuft. Bei der Inbetriebnahme
> und beim Update überträgt die Regelung selbst per UDS Daten an die
> Steuergeräte, auf denselben Adressen, die das Gateway abfragt. Eine
> Abfrage mitten in einer solchen Übertragung kann sie stören. In
> [open3e #413](https://github.com/open3e/open3e/discussions/413) meldete
> sich nach einer Inbetriebnahme mit laufendem open3e am internen Bus die
> Außeneinheit nicht mehr an, und das Bedienfeld verlangte das Service-Tool.
> Ob die Abfragen die Ursache waren, ist nicht geklärt; am internen Bus ist
> das Risiko am größten, abziehen kostet nichts. Die Firmware hält zusätzlich
> von selbst an, wenn sie solchen Verkehr sieht (siehe
> [Fremde Diagnose auf dem Bus](#fremde-diagnose-auf-dem-bus)); das ist ein
> Netz darunter und kein Ersatz.

## Läuft der Build, den ich gerade gebaut habe?

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

## Auslastung

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

## Bus-Diagnose

Auf der Statusseite. Zwei Tests, die zusammen sagen, wo es klemmt:

1. **Loopback** — der Controller sendet sich selbst ein Frame, mit
   abgeschalteter Quittung. Besteht er das nicht, ist es Firmware oder
   Pin-Konfiguration und definitiv nicht die Verkabelung.
2. **Listen-Only-Sweep** über 250, 125, 500 und 1000 kBit/s. In diesem Modus
   sendet und quittiert das Gerät nichts. Ankommende Frames beweisen
   Verkabelung *und* Bitrate. Auf einem Bus mit weiteren Knoten stört das
   nicht; hängen nur die Steuerung und dieses Gerät am Bus, fehlt der
   Steuerung für die 8 Sekunden das ACK, sie wiederholt jedes Frame im
   Dauerlauf und erholt sich danach von selbst.

| Ergebnis | Bedeutung |
|---|---|
| Loopback fehlgeschlagen | Firmware/Pins |
| Loopback ok, 0 Frames auf allen Bitraten | CAN-H/L vertauscht, falscher Strang, oder niemand sendet |
| Frames bei anderer Bitrate | Bus läuft nicht mit 250 kBit/s |
| Frames bei 250 kBit/s | Empfang ok — steigt TX trotzdem, fehlt die Quittung |

**TEC steigt, REC bleibt 0** ist dabei die aussagekräftigste Einzelbeobachtung:
auf CAN quittiert jeder andere aktive Knoten jedes Frame, also heißt das
„gesendet, niemand da".

Die Tabelle des Sweeps zeigt je Bitrate auch die **Busfehler in 2 s**. Im
Listen-Only-Modus treibt der Controller nichts auf den Bus, nicht einmal das
ACK-Bit. Fehler, die er dabei sieht, liegen auf der Leitung oder im
Empfangsweg. Zählt die Karte „CAN-Bus" im Normalbetrieb Fehler, die beim
Mithören ausbleiben, entstehen sie erst durch das eigene Treiben des Geräts,
etwa beim Quittieren; das trennt ein Problem der Leitung von einem des Moduls.
Das gilt nur, wenn noch ein dritter Knoten quittiert. Kommt beim Mithören
dasselbe Frame tausendfach hintereinander, war dieses Gerät der einzige
Empfänger, die Steuerung hat ohne ACK wiederholt, und die Fehlerzahl des
Sweeps ist dann ohne Aussage; die Diagnose sagt das auch so.

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
| Busfehler | jeden vom Controller erkannten Fehler, nach Art aufgeschlüsselt: CRC, Bit, Stuff, Form, ACK, andere | läuft seit dem Start auf, Neustart setzt ihn zurück |
| Fehler beim Empfang / Senden | auf welcher Seite der Controller stand, als er den Fehler sah | Empfangsfehler sind Frames der Gegenstelle, die beschädigt ankamen und von ihr wiederholt wurden: ein Blick auf den Signalweg zum Eingang des Boards. Sendefehler sind eigene Frames, die die Firmware wiederholt |
| Fehlerstelle im Frame | an welcher Stelle des Frames der Fehler auftrat, etwa Datenfeld oder CRC-Sequenz | Fehler, die sich auf eine Stelle häufen, sind ein Timing- oder Abtastproblem zwischen den Controllern; gleichmäßig verteilte sind Störungen auf der Leitung |
| Arbitrierung verloren | ein anderer Knoten hat im selben Bit zu senden begonnen und gewonnen | kein Fehler, das Frame wird wiederholt; kann nur passieren, während das Gerät selbst sendet, steigt also genau dann, wenn der Poller läuft. Der ESP-IDF-Treiber zählt es in seiner Summe mit, deshalb weist die Karte es getrennt aus |
| Sendewiederholungen | Frames, die nach verlorener Arbitrierung oder einem Busfehler noch einmal gesendet wurden | ein paar pro Stunde auf einem belebten Bus sind normal |
| Fehlerzähler TX / RX | TEC und REC des Controllers | springen bei einem Fehler hoch (TX um 8, RX um 1), gehen mit jedem fehlerfreien Frame um 1 zurück; ein kurzer Ausschlag ist normal und meist schon weg, bevor die Seite ihn abholt |
| Frames verpasst | Antworten auf die laufende Anfrage, die der Empfangs-Interrupt nicht mehr in die Warteschlange bekam | die einzigen Frames, die diese Firmware wirklich verworfen hat; sollte 0 sein. Broadcast-Frames der Anlage, die niemand angefordert hat, zählen nicht mit |
| Bus-Off-Recovery | wie oft der Controller nach `bus-off` neu gestartet wurde | 0 auf einem gesunden Bus |
| Bit-Timing | Zeitquanten je Bit, Abtastzeitpunkt und SJW, mit denen der Controller gerade läuft | Standard bei 250 kBit/s: 20 Quanten, 85 %, SJW 3; einstellbar unter *Einstellungen → Gerät → CAN-Bit-Timing* |

Die Aufschlüsselung liest die Firmware direkt aus dem Fehlerregister des
Controllers, weil der ESP-IDF-Treiber davon nur vier Flags weiterreicht und
einen CRC-Fehler gar nicht benennt. Hält das Register bei einem
Fehlerinterrupt gar keinen Code (es liest sich dann als „Bitfehler, Senden,
Segment 0", und Segment 0 gibt es in keinem Frame), zählt die Firmware das
nicht als Busfehler, sondern nur in `/api/status` unter `can.uncaptured`: bei
solchen Ereignissen blieb der Fehlerzähler des Controllers auf null und das
gesendete Frame kam im ersten Anlauf durch. Bis 0.3.0+b3d64f1 standen sie als
„Bit" in der Karte und als „Code 0" bei der Fehlerstelle (#262).

Ein paar Busfehler pro Minute hat fast jede Anlage; ein paar pro Sekunde sind
ein Abschluss-, Masse- oder Störungsproblem, auch wenn alle Werte ankommen.

## Bit-Timing

Der ESP-IDF-Treiber legt für 250 kBit/s 20 Zeitquanten je Bit und den
Abtastzeitpunkt bei 85 %; die Resynchronisation (SJW) setzt er auf ein
Quantum, also 200 ns. Das ist am Viessmann-Bus hinter dem Isolator des
Waveshare-Boards zu wenig: rund drei Busfehler je Sekunde, fast alle am
Frame-Ende (ACK-Slot, ACK-Delimiter, CRC-Delimiter) und auf eigenen
Sendungen, die mit SJW 3 vollständig verschwanden und beim Zurückstellen
sofort wiederkamen (vier Stunden Messreihe, ioBroker.e3oncan #262). Die
Firmware setzt den SJW deshalb auf die volle Länge des zweiten
Phasenabschnitts, bei 250 kBit/s drei Quanten. Unter *Einstellungen → Gerät
→ CAN-Bit-Timing* lassen sich Abtastzeitpunkt (50 bis 90 %) und SJW (1 bis 4
Quanten) ändern; leer heißt dieser Standard. Die Änderung gilt beim Speichern
sofort, der Controller startet dafür kurz neu, und `/api/status` zeigt unter
`can.timing`, was angewendet wurde. Ein SJW über dem Phasenabschnitt wird
gekappt. Häuft eine Anlage trotzdem Fehler am Frame-Ende, je eine Stunde mit
75 %, 80 % und 87,5 % laufen lassen und die Fehlerrate vergleichen; ein Wert,
der die Rate steigen lässt, wird einfach wieder geleert.
Ein **„no response"** oder ein fehlgeschlagener Read ist dagegen kein
Busfehler: die Anfrage wurde gesendet und quittiert, die ECU hat innerhalb der
Frist nicht geantwortet. Die beiden Dinge hängen trotzdem zusammen. Trifft ein
Busfehler genau das Anfrage-Frame, oder verliert es die Arbitrierung gegen
ein anderes Frame, muss der Sender es wiederholen. Die Firmware tut das seit
0.3.0 bis zu dreimal, so wie jeder socketcan-Adapter; davor sendete der
Controller jedes Frame genau einmal (der klassische TWAI-Controller des
ESP32-S3 kennt nur „einmal" oder „endlos"), die Anfrage ging in dem Moment
verloren, TEC zuckte kurz um 8 und eine Sekunde später stand „no response" im
Log, ohne dass die ECU je etwas gesehen hatte. Für externe Clients wie ioBroker.e3oncan
wartet `rawread` außerdem 2,5 s statt 1 s auf die Antwort, einstellbar per
`timeout` (siehe `docs/raw-gateway-api.md`), weil die HMU einer beschäftigten
Anlage gelegentlich länger braucht.

**Wichtig:** Es darf immer nur *ein* Master am Bus sein. Läuft parallel noch ein
open3e auf einem Raspberry Pi, stören sich die beiden ISO-TP-Sitzungen
gegenseitig. Zum Vergleichen abwechselnd betreiben, nie gleichzeitig.

## Fremde Diagnose auf dem Bus

Die CAN-ID einer Anfrage nennt das angesprochene Steuergerät, nicht den
Absender. Für ein Steuergerät sind das Gateway und die Regelung der Anlage
deshalb derselbe Tester, und eine eigene Absenderadresse, die das trennen
würde, gibt es in diesem Adressschema nicht. Im Normalbetrieb ist das egal:
an einer Vitocal lief am externen Bus in neunzig Sekunden kein einziges
fremdes Frame im Diagnosebereich. Bei Inbetriebnahme und Software-Update
spricht die Regelung aber selbst mit den Steuergeräten und überträgt ihnen
Daten.

Das Gateway hört deshalb im Bereich `0x680` bis `0x6FF` mit und unterscheidet
zwei Fälle:

| Gesehen | Reaktion |
|---|---|
| Sitzungswechsel (`0x10`, außer Standardsitzung), Reset (`0x11`), Routine (`0x31`), Download und Übertragung (`0x34` bis `0x37`), jeweils Anfrage, Antwort oder Ablehnung | Es wird **nichts mehr gesendet**, bis der Bus zehn Minuten frei davon war. Jedes weitere solche Frame verlängert die Pause. |
| Jede andere fremde Anfrage oder Antwort | Die nächste eigene Anfrage wartet eine Lücke von 150 ms ab, höchstens 1,5 s. |

Die Pause gilt für alles, was das Gerät sendet: zyklische Abfragen, Scan,
Lesen und Schreiben aus Weboberfläche, API und MQTT, Netzladen und
Speichermodus. Ein Scan wartet und läuft danach weiter; ein Halt endet nach
den üblichen Fehlversuchen von selbst, und der Energiemanager hat den
Sollwert zurück. Ein einzelner Lese- oder Schreibbefehl bekommt die Antwort
`paused: another device is programming a control unit`.

Auf der Statusseite steht der Zustand in der Bus-Karte unter „Fremde
Diagnose", mit Dienst und CAN-ID des auslösenden Frames und einem Knopf
„Jetzt fortsetzen" für den, der weiß, dass die Inbetriebnahme vorbei ist
(`POST /api/guard/release`). `/api/status` führt dasselbe unter `guard`.
Abschalten lässt es sich unter *Einstellungen → Bus → Passiver Empfang*;
das ist für den Fall gedacht, dass eine Anlage solche Dienste im
Normalbetrieb benutzt und das Gateway sonst dauernd schweigt.

**Grenzen.** Das erste fremde Frame sieht das Gateway erst, wenn es auf dem
Bus ist; eine eigene Anfrage kann genau damit zusammenfallen. Und es sieht
nur den Strang, an dem es hängt. Vor Inbetriebnahme und Update den Stecker
zu ziehen bleibt deshalb der sichere Weg.
