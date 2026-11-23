# Firmware-Update über die Weboberfläche

Unter *Einstellungen → Wartung* führt die Karte **Update** in drei Schritten
durch:

1. **Sicherung herunterladen.** Pflicht: ohne eine Sicherung aus der letzten
   Stunde nimmt das Gerät kein Update an.
2. **Update wählen.** Entweder *Nach Update suchen* (das Gerät fragt die
   Flash-Seite nach der stabilen Version oder dem Entwicklungsstand) oder die
   Datei `open3e-update.bin` hochladen. Sie hängt an jedem
   [Release](https://github.com/boonkerz/open3e-esp32/releases) als
   `open3e-gateway-<Version>-update.bin`, neben der Anwendung allein, dem
   Storage-Abbild, einem Factory-Abbild für die Erstinstallation per Kabel
   und `SHA256SUMS`. Wer selbst baut, erzeugt sie mit `make package`.
3. **Einspielen.** Zwei bis drei Minuten, danach lädt die Seite von selbst
   neu.

Das Paket enthält Firmware, Weboberfläche und Datenpunktdatenbank. WLAN, MQTT
und die übrigen Einstellungen stehen im NVS und sind nicht betroffen;
Datenpunkt-Auswahl, Scan-Ergebnis und Anlagenschema liegen auf der Partition,
die überschrieben wird, und werden vom Gerät selbst hinübergetragen. Die
Sicherung ist für den einen Fall da, den das nicht abdeckt: Stromausfall in
den Sekunden, in denen die Partition geschrieben wird.

Ins Internet geht das Gerät dafür nur auf den Knopfdruck *Nach Update suchen*
oder *Update einspielen*, nie von selbst, und nur zu
`esp32can.thomas-peterson.de`. Ohne Internetzugang bleibt der Weg über die
Datei.

Das Gerät hängt im Heizungsraum, deshalb drei Absicherungen:

1. **Erst prüfen, dann schreiben.** Das Paket wird vollständig empfangen und
   gegen seine Prüfsummen geprüft, bevor etwas Laufendes angefasst wird. Ein
   abgebrochener Transfer oder eine falsche Datei ändert nichts.

2. **Die laufende Firmware wird nicht angefasst.** Das Abbild geht in die
   zweite App-Partition; erst wenn es vollständig geschrieben und geprüft ist,
   wird die Boot-Partition umgeschaltet.
3. **Rollback.** Eine frisch eingespielte Firmware läuft *auf Probe*. Bestätigt
   sie sich nicht binnen 60 Sekunden als lauffähig, startet der Bootloader beim
   nächsten Reset wieder die vorherige. Der Statusbereich zeigt an, solange der
   Probelauf offen ist.

> **Nach dem Hochladen zwei Minuten nicht ausschalten und nicht neu starten.**
> Der Neustart selbst kann über eine Minute dauern: das Gerät sucht alle
> Kanäle nach dem stärksten Access Point ab, und im Heizungsraum ist der
> Empfang selten gut. Wer in dieser Zeit den Stecker zieht, weil die Seite
> nicht antwortet, löst genau den Rollback aus und hat danach wieder die alte
> Version. Die Seite wartet deshalb selbst, bis das Gerät antwortet und die
> Firmware bestätigt ist, und lädt erst dann neu. Welche Version läuft, steht
> danach oben auf der Statusseite.

Vor dem ersten Byte wird geprüft, ob die Datei überhaupt ein ESP32-Abbild ist
(Magic `0xE9`) — eine falsch ausgewählte Datei kann die Reservepartition also
nicht halb beschreiben.

## Einzelteile für die Entwicklung

Unter *Debug* liegen drei Karten, die je ein Teil einzeln ersetzen: einzelne
Webdateien, die Storage-Partition (`storage.bin`), die Anwendung
(`open3e-gateway.bin`). Auch das Storage-Abbild trägt Auswahl und Scan
inzwischen selbst hinüber. Diese Wege verlangen keine Sicherung.

Weboberfläche und Datenbank liegen auf der Storage-Partition, nicht in der
Firmware:

| Weg | Wofür | Umfang |
|---|---|---|
| *Weboberfläche aktualisieren* | einzelne Dateien aus `storage_image/www/` | wenige KiB |
| *Datenbank aktualisieren* | ganze Partition (`build/storage.bin`) | 4 MB, Neustart |

Der Datei-Weg ist der Alltagsfall, solange sich die Oberfläche noch täglich
ändert: `app.js.gz` hochladen, Seite mit Strg+F5 neu laden, fertig. Er nimmt
Dateien bis 256 KiB; die Datenbank (1,7 MB) geht nur als Partitionsabbild.
Der Versuch, sie als Datei hochzuladen, hat ein Gerät ohne Absturzbericht zum
Stillstand gebracht: littlefs schreibt in 128-Byte-Schritten mit einer
Blocklöschung je 4 KiB, neben der alten Kopie auf einer 4-MiB-Partition.

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
