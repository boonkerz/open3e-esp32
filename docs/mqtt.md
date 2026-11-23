# MQTT

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

**Retained je Datenpunkt** (Spalte „Ret." in der Tabelle, Feld `retain` in
`points.json`, Standard aus): der Broker behält den letzten Wert und liefert
ihn jedem neuen Abonnenten sofort. Ohne das sieht ein Client nach einem
Neustart einen Datenpunkt erst wieder, wenn er das nächste Mal gelesen wird,
bei einem Tagesintervall also bis zu 24 Stunden lang gar nicht (#13). Im
geflachten Modus gilt es für jedes Teil-Topic. Wird ein retained Punkt
abgewählt, deaktiviert, das Häkchen entfernt oder Topic beziehungsweise Modus
geändert, schickt das Gerät beim Speichern eine leere retained Nachricht auf
die alten Topics, damit dort kein veralteter Wert stehen bleibt; dafür muss
der Broker in dem Moment verbunden sein. open3e selbst setzt kein retained,
deshalb ist es aus, solange niemand es einschaltet.

## Kommandos

Auf `open3e/cmnd` (einstellbar) werden unter anderem diese Kommandos akzeptiert:

```json
{"mode": "read",  "data": [268, 269], "addr": "0x680"}
{"mode": "read-json", "data": [256, "0x6a1.[257,259]", "0x6a1.256.BusType"]}
{"mode": "write", "data": [[396, {"Comfort": 22.0}]], "addr": "0x680"}
{"mode": "write", "data": [["396.Comfort", 22.0]], "addr": "0x680"}
{"mode": "system"}
```

`read` und `read-json` verhalten sich hier gleich und veröffentlichen JSON,
`read-raw` die Rohbytes als Hexstring und `read-pure` die Rohbytes auf einem
`DID_<Nummer>:len=<Länge>`-Topic. `read-all` und `read-all-json` verhalten
sich ebenfalls gleich und lesen die im Scan gefundenen DIDs der adressierten
ECU. Ohne Scan oder ohne DIDs für diese ECU wird ein Fehler gemeldet. In der
lokalen Datenbank unbekannte DIDs werden als Rohdaten mit dem Namen
`DID_<Nummer>` veröffentlicht. Auch dies kann lange dauern und blockiert die
Kommando-Queue.

Adressen und Teilfelder können wie oben angegeben werden; ohne `addr` gilt die
erste gescannte ECU, sonst `0x680`. Wie bei open3e wird bei einer Teilfeld-Abfrage
der Feldname als `{didName}` verwendet: `0x680.2760.Today` veröffentlicht mit
dem Standardformat auf `open3e/Today`. Gleichnamige Felder verschiedener DIDs
können deshalb auf demselben Topic landen; `{didNumber}_{didName}` unterscheidet
sie. Das bisherige Schreibformat `{"396": 22.0}` bleibt ebenfalls möglich.
Schreiben erfordert weiterhin die Freigabe und einen als schreibbar markierten
Datenpunkt; eingestellte Grenzen bleiben wirksam.

`system` veröffentlicht nicht retained auf `open3e/system` die erkannten Geräte
und ihre beim DID-Scan gefundenen bekannten Datenpunkte mit den zugehörigen
MQTT-Topics.
Für open3e-ha zuerst einen ECU- und DID-Scan durchführen. Die MQTT-Verbindung
zum Broker und das Kommando-Topic müssen aktiv sein; die regelmäßige MQTT-
Veröffentlichung einzelner Datenpunkte über die Datenauswahl muss nicht
eingeschaltet werden. open3e-ha fordert die Werte selbst mit `read-json` an.
Bei Verwendung von open3e-ha wird empfohlen, die MQTT-Auto-Discovery der
Firmware auszuschalten, um doppelte Home-Assistant-Entitäten zu vermeiden.
Beide Integrationen können parallel laufen, wenn dies ausdrücklich gewünscht ist.

Für die mit open3e-ha übliche Topic-Benennung und zur Vermeidung von
Kollisionen wird als MQTT-Formatstring empfohlen:
`{device}_{ecuAddr:03X}_{didNumber}_{didName}`. Dieser wird von der Firmware
unterstützt. Ihr Standard bleibt `{didName}`; open3e-ha übernimmt die Topics
aus der `system`-Antwort, sodass das empfohlene Format nicht zwingend
erforderlich ist. Der Standard kann jedoch insbesondere bei mehreren ECUs
identische Topics für gleichnamige Datenpunkte erzeugen. Eine Änderung des
Formatstrings ändert die Topics auch für andere MQTT-Clients; open3e-ha danach
neu laden, damit es die aktualisierte `system`-Antwort erhält.

Fehler erscheinen auf `open3e/ERR`. Rohschreibkommandos und die experimentellen
Service-0x77-Kommandos sind über MQTT nicht verfügbar. Damit funktionieren
open3e-ha-Funktionen, die `write-raw` benötigen, nicht. Für Software mit eigener
Datenpunkt-Datenbank gibt es einen separaten Rohdaten-Zugang, siehe
[Rohdaten für externe Integrationen](rohdaten.md).

## Home Assistant

Auto-Discovery ist zuschaltbar. Pro Blattwert entsteht eine Entität mit Einheit
und passender `device_class` direkt aus dem open3e-Codec. Beim Abschalten werden
die Discovery-Topics wieder geleert, damit keine verwaisten Entitäten
zurückbleiben.

Schreibbare Zahlen werden als `number` angemeldet, Aufzählungen als `select`.
Die Spalte **Beschriftungen** macht aus einer nackten Zahl eine Auswahlliste
(`0=zu, 1=offen`), die Spalte **Grenzen** (`10..60`) begrenzt den Bereich einer
Zahl; beides gilt auch beim Schreiben, siehe [Schreiben auf den
Bus](schreiben.md).
