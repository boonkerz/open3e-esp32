# Schreiben auf den Bus

Zwei Sperren, weil am anderen Ende eine echte Heizung hängt:

1. **Global aus.** „Schreiben freigeben" muss in den Einstellungen aktiviert
   werden. Standard ist deaktiviert.
2. **Pro Datenpunkt.** Geschrieben wird nur, was die open3e-Datenbank als `rw`
   führt.
3. **Grenzen je Datenpunkt** (Spalte „Grenzen" in der Tabelle, Feld `range`
   in `points.json`), optional. Die Datenbank kennt für eine Zahl nur die
   Breite des Feldes, deshalb bietet Home Assistant einen Warmwasser-Sollwert
   von -3276,8 bis 3276,7 °C an (#21). Mit `10..60` in der Spalte meldet die
   Discovery genau diesen Bereich als `min`/`max`, und jeder Schreibbefehl
   außerhalb wird abgelehnt, egal ob er aus Home Assistant, vom Befehlskanal,
   von der API oder aus der Weboberfläche kommt; auch das Erzwingen hebt die
   Grenze nicht auf. Eine Seite darf fehlen (`..60`), ein vorangestellter
   Feldname (`Comfort: 10..60`) beschränkt die Grenze auf dieses Feld, sonst
   gilt sie für jede Zahl des Datenpunkts. Geprüft wird, was der Befehl
   setzt; Felder, die die Firmware aus dem aktuellen Wert ergänzt, zählen
   nicht. Die Anlage prüft ihrerseits und lehnt Unsinn mit
   `requestOutOfRange` ab, aber eine Grenze, die man selbst gezogen hat,
   greift früher und sichtbarer.

Vor jedem Schreiben liest die Firmware den Datenpunkt zuerst — das bestätigt,
dass er auf dieser ECU existiert, und die Antwortlänge entscheidet, welche
Codec-Variante gilt.

Datenpunkte, die open3e selbst nicht kodieren kann (Text-, MAC-, Datumsfelder:
`not implemented yet`), werden abgelehnt statt mit einer selbst erfundenen
Kodierung geschrieben.

## Heizkennlinie

Neigung und Niveau eines Heizkreises stehen in `MixerOneCircuitCentralHeatingCurve`
(DID 880, für die Kreise zwei bis acht 881 bis 887), Felder `Gradient` und
`Level`. open3e führt sie als nur lesbar; eine Vitocal 250-A hat das Schreiben
über den Bus angenommen, deshalb markiert die Datenbank dieses Geräts sie als
`rw`. Mit „Schreiben freigeben" entstehen in Home Assistant Regler dafür.
Die Datenbank kennt nur die Feldbreite, also Grenzen setzen: in der Spalte
„Grenzen" etwa `Gradient: 0.2..3.5` für die Neigung, und für das Niveau
`Level: -13..40`, beides in einem Feld mit Semikolon getrennt
(`Gradient: 0.2..3.5; Level: -13..40`). Das dritte Feld `BasePoint` zeigt die
App nicht; es bleibt besser, wie es ist.

Einfacher geht es im Reiter *Anlage*: die Karte **Steuern** listet je
erkanntem Heizkreis Kennlinie, Raumtemperatur-Sollwerte und Betriebsart, dazu
Warmwasser-Solltemperatur und -Betriebsart, und übernimmt die angehakten
Punkte mit Home-Assistant-Häkchen und Grenzen in die Auswahl.

**Die Linie als Bild.** Zwei Zahlen sagen wenig, solange man die Linie nicht
sieht, die sie beschreiben. Wer die Heizkennlinie eines Kreises auswählt,
bekommt deshalb ihr Bild dazu, so wie die App es zeichnet: Vorlauftemperatur
über der Außentemperatur von +20 bis −30 °C, die sechs Stützpunkte
beschriftet, die Vorlaufgrenzen des Kreises als Begrenzung. Dazu der Punkt,
an dem die Anlage gerade steht, mit ihrem eigenen Vorlauf-Sollwert. Gezeichnet
wird für den Raum-Sollwert, der gerade gilt, nachts also die abgesenkte Linie.

| Topic | Inhalt |
|---|---|
| `<Basis>/curve/<n>` | das SVG, retained, `n` ist der Heizkreis ab 1 |
| `<Basis>/curve/<n>/b64` | dasselbe Base64-codiert, für Grafana (`data:image/svg+xml;base64,` davor) |

Mit Auto-Discovery erscheint es in Home Assistant als Bild-Entität
„Heizkennlinie <Name des Kreises>" und lässt sich mit einer Bild-Karte neben
die beiden Regler setzen. Es wird einmal pro Minute neu gerechnet und nur
gesendet, wenn es sich geändert hat. Ein Diagramm zum Anfassen kann die
Discovery nicht ausliefern; die Regler bleiben die Bedienung, das Bild zeigt
die Wirkung. Wird der Datenpunkt abgewählt, verschwindet die Entität.

Die Formel ist die, die Viessmann für seine witterungsgeführten Regelungen
angibt. Sie trifft die Beschriftung der App (Neigung 0,2, Niveau 0, Raum
23 °C: 24, 28 und 34 °C bei +20, 0 und −30 °C) und den Sollwert der Anlage
selbst (27,2 °C bei 4,2 °C außen, gerechnet 27,24).

Zugeordnet, aber über den Bus nicht schreibbar, an derselben Anlage geprüft:

| Einstellung in der App | Datenpunkt | Schreiben |
|---|---|---|
| Regelungsbetrieb (deaktiviert, monovalent, bivalent parallel, bivalent alternativ) | 2404 `BivalenceControlMode`, Feld `OperationMode`: 0 deaktiviert, 2 bivalent parallel | Anlage lehnt ab (`conditionsNotCorrect`) |
| Name des Heizkreises | 627 `CentralHeatingOneCircuitName` | Textfeld, nicht kodierbar |
| Intelligente Pumpensteuerung | nicht gefunden | in keinem der gescannten Datenpunkte sichtbar |

Raum-Sollwerte (424), Betriebsart (1415) und Zeitprogramm (761 bis 767) waren
schon vorher schreibbar.
