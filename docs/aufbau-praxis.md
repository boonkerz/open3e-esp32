# Aufbau in der Praxis: Waveshare-Modul an einer Vitodens

Ein Erfahrungsbericht von **wunderbaum** aus der
[open3e-Diskussion #386](https://github.com/open3e/open3e/discussions/386),
hier gesammelt, weil er das beschreibt, was in der README nur als Regel
steht: wie man das Ding tatsächlich an die Heizung bekommt. Die Anlage ist
eine Vitodens 343-F B3UG-19, eine Brennwerttherme für Fußbodenheizung mit
Solarthermie fürs Brauchwasser. Die Fotos stammen von ihm.

Was hier steht, ergänzt die beiden anderen Artikel und wiederholt sie nicht:
die technische Einordnung in [open3e-wiki.md](open3e-wiki.md), die
Verkabelungsregeln (Masse, Abschluss, Spannung) in der
[README](../README.md#hardware).

## Was gebraucht wird

| Teil | Beispiel | Anmerkung |
|---|---|---|
| Waveshare ESP32-S3-RS485-CAN | ca. 20 € bei Waveshare, ca. 30 € mit Tageslieferung bei Amazon | das Board, auf das diese Firmware zugeschnitten ist |
| Hutschienennetzteil | Mean Well HDR-15-12 | 12 V reichen, das Board nimmt 7 bis 36 V |
| Leitungsschutzschalter | B6 oder B16, einpolig | der guten Ordnung halber vor dem Netzteil |
| Kleine Unterverteilung aus Kunststoff | Aufputz, 4 bis 6 TE | Kunststoff, sonst funkt das WLAN nicht heraus |
| Stecker für Buchse 91 | Phoenix Contact MSTB 2,5/3-ST-5,08 | siehe unten, das Original ist unverhältnismäßig teuer |
| Verdrahtung | ein Rest Patchkabel | verdrilltes Paar für CAN-H/CAN-L |

Zur Unterverteilung eine Warnung aus Erfahrung: bei der verwendeten waren
die Schrauben der eingebauten Klemmleisten aus einer sehr weichen Legierung
und ab Werk so fest angezogen, dass man sie kaum ohne Schaden herausbekommt.
Beim Kauf auf brauchbare Klemmen achten.

## Vorher flashen

Das Board am PC flashen, bevor es eingebaut wird. Im Browser geht das über
<https://esp32can.thomas-peterson.de>, danach WLAN über den Hotspot
einrichten. Steckt das Board erst einmal in der Verteilung neben der Heizung,
ist der USB-Anschluss schlecht erreichbar; Updates gehen danach per OTA aus
der Weboberfläche.

## Der Aufbau

![Unterverteilung mit Waveshare-Modul, Netzteil und Leitungsschutzschalter](img/aufbau/aufbau-unterverteilung.jpg)

Von links nach rechts: das Waveshare-Modul, das Hutschienennetzteil, der
Leitungsschutzschalter. Die Stromversorgung für das Netzteil kommt vom
Anschlusskasten der Heizung. Oben die beiden Klemmleisten der Verteilung,
die linke ist als **CAN-GND** beschriftet.

Auf der linken Klemmleiste treffen sich GND des Boards, der Minuspol des
Netzteils und die GND-Ader zum Stecker 91. Diese Masseverbindung stammt aus
einer früheren Fassung der README, die R31 als Brücke zwischen Board-Masse
und CAN-Masse las. Nach dem Schaltplan ist der CAN-Teil galvanisch getrennt,
die Verbindung erreicht den Transceiver nicht und ist für den Bus wirkungslos
(Issue #14). Sie schadet nicht, nötig ist sie aber nicht; die GND-Ader zum
Stecker 91 kann entfallen. Verdrahtet ist alles mit einem Rest Patchkabel,
für CAN-L und CAN-H eines der verdrillten Paare, hier orange und orange-weiß.

Als Schaltbild:

![Schaltbild: Versorgung, Leitungsschutzschalter, Netzteil, Adapter, Steckverbinder](img/aufbau/schaltbild.png)

## Der gelbe Stecker 91

Der CAN-Anschluss der Heizung ist die gelbe Buchse **91** unter der Haube,
beschriftet mit **L, GND, H**. Viessmann verkauft den passenden Stecker nur
im Paket mit sechs anderen, für rund 22 €. Für einen einzelnen Stecker reicht
ein handelsüblicher Leiterplattensteckverbinder im Raster 5,08 mm mit drei
Polen:

- Phoenix Contact [MSTB 2,5/3-ST-5,08](https://www.phoenixcontact.com/de-de/produkte/leiterplattenstecker-mstb-25-3-st-508-1757022)
  (passt, mit Firmenadresse gibt es dort auch Muster)
- die [Leiterplattenanschlussklemme 3-polig, RM 5,08 mm, von Pollin](https://www.pollin.de/p/leiterplattenanschlussklemme-3pol-rm5-08-mm-10a-452654)
  (passt laut einem Bericht aus dem Home-Assistant-Forum, Diskussion #9)
- vermutlich ebenso der [von Reichelt](https://www.reichelt.de/de/de/shop/produkt/steckbare_schraubklemme_-_3-pol_rm_5_mm_0_-292638),
  ungetestet

Vermutlich passt jeder dreipolige Leiterplattensteckverbinder im Raster
5,08 mm; die Liste nennt nur die, von denen es Rückmeldungen gibt.

| Original | Phoenix Contact |
|---|---|
| ![Originalstecker von Viessmann](img/aufbau/stecker-original.png) | ![Phoenix Contact MSTB 2,5/3-ST-5,08](img/aufbau/stecker-phoenix-produkt.png) |

Dem Phoenix-Stecker fehlen nur die Erhöhungen, wo der Originalstecker
Vertiefungen hat. Er passt ohne Nacharbeit, nichts muss gefeilt werden. Die
Führungsschienen passen nicht hundertprozentig, weil beim gelben Stecker eine
davon gefüllt ist, der Stecker sitzt trotzdem sehr fest. Die Beschriftung muss
man selbst anbringen:

![Phoenix-Stecker, beschriftet mit L, G, H, auf einem Ausdruck mit dem Originalstecker](img/aufbau/stecker-phoenix.jpg)

Das Schwarzweißbild darunter ist der Ausdruck eines PDFs aus dem
Viessmann-Forum, das den Originalstecker zeigt. Der grüne Stecker liegt zum
Vergleich darauf.

## Abschlusswiderstand

![Der kleine Schalter für den 120-Ω-Abschluss auf dem Board](img/aufbau/abschluss-schalter.jpg)

Hängt sonst nichts weiter am CAN-Bus der Heizung, muss der kleine Schalter auf
dem Board auf **ON** stehen, das Gerät sitzt dann am Busende. Hängen schon
andere Teilnehmer daran, bitte den Abschnitt zum Abschluss in der README
lesen: zwei Abschlüsse zu viel legen den Bus lahm.

## Inbetriebnahme

Ist alles verdrahtet, zuerst in der Weboberfläche die **Busdiagnose**
drücken:

![Busdiagnose in der Weboberfläche](img/aufbau/web-busdiagnose.png)

Zeigt sie Fehler, stimmt die Verkabelung noch nicht, meist der Abschluss oder
vertauschte Adern. Erst dann den **Bus-Scan** auf dem Reiter *System* starten; er
dauert einige Minuten:

![Bus-Scan auf dem Reiter System](img/aufbau/web-busscan.png)

Danach, sofern MQTT konfiguriert ist, die Datenpunkte auswählen, die in Home
Assistant erscheinen sollen:

![Datenpunktauswahl](img/aufbau/web-datenpunkte.png)

Hier nicht übertreiben. Voreingestellt wird jeder gewählte Datenpunkt alle
60 Sekunden abgefragt. Bei Werten, die sich nie ändern, etwa der
Bus-Identifikation, reicht einmal am Tag; das Intervall lässt sich je
Datenpunkt setzen.

*Auswahl speichern* veröffentlicht die Datenpunkte per MQTT, nachprüfbar etwa
mit dem MQTT Explorer:

![Die Topics im MQTT Explorer](img/aufbau/web-mqtt-explorer.png)

Dank Auto-Discovery tauchen sie dann auch in Home Assistant auf. Schreiben
ist in der Firmware voreingestellt gesperrt; wer Datenpunkte setzen will,
gibt es in den Einstellungen frei, siehe README.
