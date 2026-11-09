# open3e auf dem ESP32 (Waveshare ESP32-S3-RS485-CAN)

Eine Firmware, die E3-Geräte direkt am CAN-Bus ausliest und nach MQTT bringt —
auf einem Board für rund 20 €, das an 7–36 V direkt im Heizungsraum läuft.
Kein Raspberry Pi, kein Python, kein SocketCAN.

Zur Masse: der CAN-Teil des Boards ist galvanisch getrennt, die CAN-Klemme hat
nur CAN-H und CAN-L. R31 (0 Ω) verbindet die CAN-Masse laut Schaltplan nur
mit dem Ableitpunkt der RS485-Schutzdioden, nicht mit GND des Boards. Eine
Busmasse an GND erreicht den Transceiver also nicht; am Stecker 91 werden nur
CAN-H und CAN-L belegt, wie es auch die Viessmann-Anleitung vorsieht.

Projektseite: <https://github.com/boonkerz/open3e-esp32>

## Verhältnis zu open3e

Das ist kein eigenes Protokoll und kein Konkurrenzprodukt, sondern open3e auf
anderer Hardware:

- Die **Codecs sind ein Port** von Open3Ecodecs.py nach C.
- Die **Datenpunktdatenbank kommt aus open3e** und wird beim Bauen aus dem
  Repository erzeugt (derzeit 1564 Datenpunkte).
- **Topics, Payloads, LWT und Kommando-Topic folgen Open3Eclient.py**,
  einschließlich der Formatstring-Platzhalter `{didName}`, `{didNumber}`,
  `{ecuAddr}` und `{device}`.

Ein bestehendes Broker-Setup, ein Home-Assistant-Template oder eine
Node-RED-Strecke läuft also unverändert weiter. Wer vom Pi umzieht, tauscht die
Hardware, nicht die Integration.

## Warum man den Werten trauen kann

Das ist die eigentliche Frage bei einer Neuimplementierung: eine Abweichung im
Codec merkt man nicht am Fehler, sondern erst daran, dass ein Wert still falsch
ist.

Deshalb werden die Testvektoren nicht von Hand geschrieben, sondern erzeugt,
indem **open3es eigenes Python läuft**. Verglichen wird byteweise:

| Prüfung | Ergebnis |
|---|---|
| Dekodieren == open3e | 4692 / 4692 Vektoren über alle 1564 DIDs |
| Kodieren == open3e | 3990 / 3990 Vektoren |
| … und dieselben Ablehnungen | 332 / 332 |
| geflachtes MQTT == `mqttdump()` | 4692 Datenpunkte, 23 395 Topic/Wert-Paare, 0 Abweichungen |

Dazu kommen eigene Suiten für ISO-TP (Segmentierung, Flow Control,
SN-Überlauf, Frame-Verlust bis 4095 Byte) und für UDS — letztere sichert
ausdrücklich die Eigenschaft, dass eine Anfrage, die **nicht** oder negativ
beantwortet wird, keinen Erfolg meldet.

Die E380-Dekodierung ist aus [E3onCAN](https://github.com/MyHomeMyData/E3onCAN)
portiert und wird gegen dessen Ausgabe geprüft (560 / 560 Vektoren über 14
CAN-IDs).

## Ausprobieren

Zum Ausprobieren braucht es **keine Toolchain**. Die Firmware lässt sich aus
dem Browser aufspielen — über Web Serial, also mit Chrome, Edge, Opera,
Firefox ab 151 oder Chrome für Android. Mit Safari und auf iOS geht es nicht,
weil WebKit die Schnittstelle nicht umsetzt:

<https://esp32can.thomas-peterson.de>

Danach spannt das Gerät einen WLAN-Hotspot auf, das Captive Portal öffnet die
Einrichtungsseite von selbst. WLAN eintragen → Neustart → erreichbar unter
`http://open3e.local`. Ab hier passiert alles im Browser.

Wie der Einbau neben der Heizung konkret aussieht, von der Unterverteilung
bis zum Stecker für die gelbe Buchse 91, steht mit Fotos in
[aufbau-praxis.md](https://github.com/boonkerz/open3e-esp32/blob/main/docs/aufbau-praxis.md).

## Der Bus-Scan

Das Gegenstück zu `Open3E_depictSystem` auf dem Pi, nur dass das Ergebnis auf
dem Gerät bleibt und die Oberfläche damit weiterarbeitet.

| Modus | Umfang | Dauer |
|---|---|---|
| Schnellscan | nur die Datenpunkte der mitgelieferten Datenbank | ca. 1 Min. je ECU |
| Vollscan | DID 256 bis 4000 | 10–20 Min. je ECU |

Gesucht wird auf den COB-IDs `0x680`–`0x6EF` über **DID 256**
(`BusIdentification`); dazu kommt **DID 377** für die Viessmann-Identnummer.
Daraus füllt sich eine Gerätetabelle mit Typ (`HPMUMASTER`, `EMCUSLAVE`, …),
Funktion, Bustyp, Software- und Hardwarestand und Seriennummer.

Zwei Dinge, die den Unterschied machen:

**DID 256 wird über den echten Codec dekodiert**, nicht über feste
Byte-Offsets. Der Datensatz hat eine Längenvariante — handgerechnete Offsets
produzieren auf einem Gerät, das die andere zurückgibt, stillschweigend
Unsinn.

**Das Zwischenergebnis wird nach jeder ECU gespeichert.** Ein Neustart mitten
im Vollscan wirft nicht alles weg.

Jede ECU bekommt einen frei wählbaren **Namen**, der den Platzhalter
`{device}` in den Topics füllt — genau wie der Schlüssel in open3es
`devices.json`. Voreingestellt ist deshalb `0x680` und nicht der Gerätetyp:
ein Formatstring mit `{device}` erzeugt so dieselben Topics wie vorher auf dem
Pi. Vergebene Namen überleben einen erneuten Scan.

### „nicht in DB"

Ein Datenpunkt landet nur in der Liste, wenn die ECU mit einer **positiven
UDS-Antwort** geantwortet hat. Die Markierung heißt also nicht „kam nichts
zurück", sondern „open3e liefert für diesen DID keine Beschreibung mit"; die
Antwortlänge steht als Beleg daneben.

Lesen und nach MQTT senden lassen sich solche Datenpunkte trotzdem — der Wert
ist dann ein Hex-String wie in open3es Raw-Modus. **Schreiben ist für sie
gesperrt.** Wer so einen DID identifiziert, kann ihn bei open3e beitragen.

## Datenpunkte auswählen

Pro Datenpunkt wird eingestellt, ob er überhaupt gesendet wird, in welchem
Intervall, und wie:

| Modus | Ergebnis |
|---|---|
| **JSON** | ein Topic, Nutzlast das ganze Objekt |
| **geflacht** | ein Topic je Unterfeld, Nutzlast der nackte Wert |

```
open3e/FlowTemperatureSensor
  → {"Actual": 27.2, "Minimum": 21.0, "Maximum": 31.4, …}

open3e/FlowTemperatureSensor/Actual   → 27.2
open3e/FlowTemperatureSensor/Minimum  → 21.0
```

Das Topic-Suffix lässt sich je Datenpunkt überschreiben, und einzeln
abschalten, ob er in die Home-Assistant-Discovery geht. `open3e/LWT` trägt
retained `online` / `offline`.

Für Aufzählungen lassen sich eigene Beschriftungen hinterlegen — aus
„BypassStatus 2" wird dann „automatisch", und in Home Assistant entsteht statt
eines Zahlenfelds eine Auswahlliste.

## Was man sieht

Eine Statusseite mit WLAN, Laufzeit, Speicher und MQTT-Zählern — und einer
CAN-Diagnose, die mehr zeigt als „läuft": Bus-Fehler, Sende- und
Empfangsfehlerzähler, Wiederanläufe. Die beiden Fehlerzähler steigen lange
bevor der Controller wirklich bus-off geht und sind damit das früheste
sichtbare Zeichen für ein Verkabelungs- oder Bitraten-Problem.

Dazu ein **CAN-Mitschnitt im Browser**, der unbeaufsichtigt auf ein Ereignis
warten kann. Drei Auslöser:

- ein **UDS-Schreibzugriff** irgendwo auf dem Bus,
- etwas **Neues**: der Mitschnitt lernt eine Zeit lang, welche Identifier
  vorkommen und welche Bytes sich überhaupt je ändern, und löst dann bei einem
  unbekannten Identifier aus — oder bei einem Byte, das durchgehend konstant
  war und es plötzlich nicht mehr ist,
- eine **Änderung an den Steuer-Datenpunkten**, mit denen das Backend den
  Speicher fährt. Die liegen in ISO-TP-Nachrichten mit Service 0x77, also
  mehrere Frames tief; ein Byte-Vergleich auf einem einzelnen Frame sagt dort
  nichts.

Damit lässt sich herausfinden, was ein Hersteller-Gateway tut, während niemand
davorsitzt — und genau so ist das netzdienstliche Laden unten gefunden
worden.

## Schreiben auf den Bus

Zwei Sperren, beide müssen offen sein: ein globaler Schalter in den
Einstellungen (ab Werk zu), und die open3e-Datenbank muss den Datenpunkt als
`rw` führen. Datenpunkte ohne Beschreibung lassen sich grundsätzlich nicht
schreiben.

Vor jedem Schreibvorgang wird der Datenpunkt gelesen — das wählt die
Codec-Variante und bestätigt, dass es ihn auf dieser ECU überhaupt gibt. Felder,
die der Aufrufer nicht angibt, werden aus dem aktuellen Wert ergänzt; ein
Bedienelement kann also eine Lüfterstufe setzen, ohne die übrigen Felder des
Datensatzes zu kennen.

## Was darüber hinaus geht

**Home Assistant.** MQTT-Auto-Discovery mit Einheiten und Geräteklassen aus
dem Codec; schreibbare Datenpunkte werden zu Bedienelementen statt zu
Anzeigen.

**E380-Energiezähler**, passiv mitgelesen — der Zähler beantwortet keine
Anfrage, er sendet acht Byte auf `0x250`–`0x25D`, und das ist das ganze
Protokoll.

**Broadcast-Kanal (Service 0x77).** Auf einem Vitocharge-Bus läuft darüber der
gesamte Verkehr zwischen Backend-Gateway und Speicher. Die Firmware setzt die
ISO-TP-Fragmente zusammen und dekodiert sie mit demselben Codec wie alles
andere.

**Kontakteingänge.** Die beiden freien GPIO des SH1.0-Steckers lesen einen
Schalter — Klingel, Türkontakt, Störmeldung — und melden ihn als binären Sensor
nach Home Assistant. Die Entprellung ist unsymmetrisch ausgelegt, damit auch
ein über einen Optokoppler abgegriffenes Wechselspannungssignal als *ein*
Ereignis ankommt und nicht als hundert.

**KNXnet/IP.** Derselbe Kontakt kann zusätzlich ein 1-Bit-Telegramm auf eine
Gruppenadresse schreiben, wahlweise per Tunnelling oder Routing.

**evcc.** Das Gerät erzeugt den fertigen Konfigurationsblock für evcc aus der
tatsächlichen Datenpunktauswahl — Netz, PV, Speicher, Ladestand und die
Batteriesteuerung über `batterymode`.

**Netzdienstliches Laden einer Vitocharge VX3.** Über DID 2188 lässt sich der
Sollwert am Netzverknüpfungspunkt vorgeben. Der Energiemanager der Anlage
schreibt denselben Datenpunkt alle zehn Sekunden neu; die Firmware übertönt ihn
und wird dabei vom Broadcast-Kanal geweckt, statt auf einen Timer zu warten.
Nichts wird dauerhaft umkonfiguriert: jede Frist, jeder Neustart und jedes
gezogene Kabel beenden den Eingriff, und die Anlage regelt binnen Sekunden
wieder selbst.

Einzelheiten zu all dem stehen in der README des Projekts.

## Rohdaten für eigene Decoder

`GET /api/rawread` und `POST /api/rawwrite` liefern und schreiben
UDS-Nutzdaten **ohne** den open3e-Codec, und ausgewählte CAN-IDs lassen sich
roh nach `<Basis>/raw/<id>` weiterreichen. Gedacht für Software mit eigener
Datenpunktdatenbank — namentlich ioBroker.e3oncan im Gateway-Betrieb.

Diese Schnittstelle stammt aus einem
[Pull Request von MyHomeMyData](https://github.com/boonkerz/open3e-esp32/pull/1)
und ist seine Arbeit, nicht meine.

## Was es nicht kann

- **RS485** — die Hardware ist da, die Firmware nutzt sie nicht.
- **DoIP** als Alternative zu CAN.
- **Schreiben über Service 0x77.** Gelesen und dekodiert wird er vollständig;
  selbst schreiben ginge nur gegen ein Gateway, das dieselben Datenpunkte alle
  zehn Sekunden neu setzt.
- **KNX Secure** — eine gesicherte Installation ignoriert, was gesendet wird.
- **Ein ESP32 ist kein Linux.** Wer neben open3e eigene Skripte, eine
  Datenbank oder weitere Adapter auf derselben Kiste laufen lässt, ist mit dem
  Raspberry Pi besser bedient. Dieses Gerät macht eine Sache.

## Lizenz und Herkunft

Das Projekt steht unter der **Apache License 2.0** — derselben Lizenz wie
open3e und E3onCAN, aus denen es schöpft. Der Code in `main/` ist ein
eigenständiger Port; die Datenpunktdefinitionen, Enumerationen und die
Codec-Semantik stammen aus open3e (Copyright 2023 abnoname, philippoo66 und
Mitwirkende), die E380-Dekodierung aus E3onCAN (Copyright 2023 MyHomeMyData).
Was woher kommt, steht in der `NOTICE` des Projekts. Die open3e-Quellen liegen nicht im
Repository — `tools/fetch_open3e.py` holt sie beim Bauen auf einen festen
Commit und `tools/gen_dpdb.py` wandelt sie in die Datenbank auf dem Gerät.
