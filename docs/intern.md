# Wie das intern zusammenhängt

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
