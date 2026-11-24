# Anlage

Der Reiter *Anlage* zeichnet den Kältekreis und die Hydraulik einer
Wärmepumpe nach dem Vorbild der Viessmann-Serviceansicht, mit Live-Werten:
Außentemperatur und Lüfter, Verdampfer, Expansionsventil, Verdichter mit Saug-
und Hochdruckseite (Temperatur und Druck), Öltemperatur, Vierwegeventil,
Verflüssiger mit Wärme- und elektrischer Leistung, Pumpe, Vor- und Rücklauf,
Druck, Durchfluss, Warmwasserspeicher, Heizstab, Puffer und Heizkreise.

An einem **Gas-Brennwertgerät** (kein Verdichter, aber Brenner-Datenpunkte
am Master) steht an Stelle des Kältekreises ein Kessel mit Modulation,
Abgastemperatur, Gebläsedrehzahl, Brennerstarts und Brennerstunden, daneben
die gedämpfte Außentemperatur; die Sensoren der Wasserseite stehen dort, wo
sie im Gerät sitzen. Gebaut nach den Daten einer Vitodens 300-W, an einer Vitodens
333-F bestätigt.

Hängt ein **Vitocharge** am Bus, kommt eine zweite Karte *Stromspeicher und
Photovoltaik* dazu: die Strings mit Leistung und Spannung, die PV-Summe, der
Wechselrichter, die Batterie mit Ladestand und ihren einzelnen Modulen
(Spannung, Temperatur, Spanne der Zellspannungen), der Netzanschlusspunkt
mit „Netzbezug" oder „Einspeisung" und das Haus mit seinem Verbrauch. Die
Modulwerte liest der Browser aus Rohdaten (DID 1577–1582), deren Aufbau an
einer einzelnen VX3 abgelesen ist. Diese Karte gibt es nur im Browser, nicht
als Bild für Home Assistant.

**Was der Bus über die Anlage verrät.** Den Modellnamen nicht: DID 377 und
2829 enthalten nur die Seriennummer, 963 (Ersatzteilnummer) wird abgelehnt.
Lesbar sind die Nennwärmeleistung (2342), Verdichtertyp (2570) und
Kältemittel (2568), also „Vitocal, 6 kW". Die **Konfiguration** dagegen lässt
sich erkennen, aus drei Quellen:

1. **Rollen der Steuergeräte** (DID 256): Innengerät ist der `HPMUMASTER`
   (Wärmepumpe) oder `HMUMASTER` (Kessel), Außeneinheit, wer den Verdichter
   (2346) meldet, bei einer Split-Anlage die `VCMU`, bei einem Monoblock der
   Master selbst; `EHCU` ist der Heizstab, `EMCU` der Stromspeicher.
   Backend-Gateway und Raumregelung zählen dabei nicht: sie beantworten an
   manchen Anlagen jeden Datenpunkt der Datenbank, mit Nullen.
2. **Was der Scan nicht auflistet, gibt es nicht.** Puffer-, Solar- oder
   Durchflusssensor-Datenpunkte, die eine Anlage nicht hat, beantwortet sie
   mit `conditionsNotCorrect` und fehlen im Scan.
3. **Was antwortet, ist noch nicht verbaut.** Eine Vitocal beantwortet die
   Sensoren aller vier Heizkreise und der Puffer; nicht belegte melden
   `SensorStatus` **`not_available`** (ID 4) und −3276,8 als Wert, belegte
   `no_error`. Dazu kommen Konfigurations-Datenpunkte wie 2574 (Leistung des
   Heizstabs), 933–936 (Art des Heizkreises) und 1415–1418 (Betriebszustand).
   Ein zweiter Lüfter meldet 255, wenn es ihn nicht gibt.

Die **Sondierung** („Anlage erkennen", beim ersten Öffnen des Reiters und
nach jedem Scan automatisch) liest dafür rund 70 Datenpunkte einmalig,
nacheinander, und legt die Rohantworten unter `/api/plant` ab
(`/data/plant.json`, in Sicherung und Wiederherstellung enthalten). Die
Regeln, die daraus das Profil und das Bild machen, liegen in der
Weboberfläche, nicht in der Firmware: sie lassen sich ohne Firmware-Update
verbessern, und das Gerät selbst deutet nichts.

Die Werte im Bild werden **nacheinander** gelesen, ein Datenpunkt je Sekunde,
nur solange der Reiter offen ist; jeder Lesevorgang ist eine Busrunde im
HTTP-Server, zwei gleichzeitig würden sich nur gegenseitig aufhalten. Die
Leitungen zeigen die **Fließrichtung** im Heizbetrieb als wandernde Striche:
Kältemittel, solange der Verdichter läuft, Heizwasser, solange die Pumpe
läuft, die Geschwindigkeit folgt der jeweiligen Prozentzahl; bei 0 % steht
die Leitung. Im Hintergrundbild für Home Assistant läuft die Animation
immer, weil das Bild von den Werten nichts weiß.

**Home Assistant.** Dasselbe Bild gibt es als Picture-Elements-Karte: der
Knopf *Datenpunkte des Schemas aufnehmen* ergänzt die Auswahl um die
Datenpunkte des Bildes (15 s, JSON, HA-Häkchen), *Karte erzeugen* speichert
das leere Schema auf dem Gerät, veröffentlicht es retained auf
`<Basis>/plant/svg` und meldet es per Discovery als Image-Entität
`image.<Basis-Topic>_anlagenschema` an; die YAML der `picture-elements`-Karte
nutzt `image_entity`, so liefert Home Assistant das Bild selbst aus, auch von
unterwegs und auf einer HTTPS-Seite, wo die Adresse des Gateways nicht
erreichbar wäre. Jeder Wert ist ein `state-label` der per Discovery
angelegten Sensoren, an derselben Stelle wie im Reiter. Die Entitäts-IDs
folgen der Regel von Home Assistant (`sensor.<Basis-Topic>_<Name>`); weicht
eine ab, etwa nach einem Namenskonflikt, wird sie in der YAML angepasst.
**Fertiges Bild mit Werten.** Wer kein Home Assistant hat, bekommt das
Schema trotzdem: *Karte erzeugen* legt zusätzlich eine Vorlage mit
Platzhaltern und die Liste der Datenpunkte dahinter auf dem Gerät ab
(`PUT /api/plant/live`, `PUT /api/plant/slots`). Die Firmware liest die
Werte jede Minute nacheinander vom Bus, setzt sie ein und veröffentlicht das
fertige SVG retained auf `<Basis>/plant/live` (auch als Image-Entität
`image.<Basis-Topic>_anlagenschema_live` angemeldet), dasselbe Base64-codiert
auf `<Basis>/plant/live/b64` für Bild-Panels, die eine Data-URL erwarten
(`data:image/svg+xml;base64,…`). Grafana zeigt es
beispielsweise über ein Bild-Panel, das Node-RED aus dem Topic befüllt, eine
Wandanzeige abonniert das Topic direkt. Zahlen kommen mit Dezimalkomma und
Einheit wie im Reiter, Zustände als Wort.

Damit das Bild in Home Assistant nicht läuft, während die Anlage steht,
liest die Firmware alle 30 s Verdichterdrehzahl (2346) und Pumpe (381) und
veröffentlicht das Bild bei einem Wechsel neu, mit stillgelegter Animation
für den stehenden Kreis. Ohne MQTT, oder falls Home Assistant das SVG in der
Image-Entität nicht anzeigt: *SVG herunterladen*, die Datei nach `config/www/plant.svg` legen
und `image: /local/plant.svg` verwenden. Im LAN tut es auch
`http://<Gateway>/plant.svg`; ein neues `storage.bin` löscht die Datei,
*Karte erzeugen* legt sie wieder an.

**Entschlüsselt: DID 1238** `AvailableActorSensorComponents` ist eine Zählung
und dann je Aktor die Nummer seines Datenpunkts als 16 Bit (Little Endian):
auf einer 252-A `07 7d01 3207 ee06 ef06 8401 8601 eb01` = 381 Heizkreispumpe,
1842 Umschaltventil, 1774 Kurbelwannenheizung, 1775 Lüfter 1, 388 und 390
Expansionsventile, 491 Zirkulationspumpe. Das Profil listet sie, und
Umschaltventil und Zirkulationspumpe kommen daraus ins Bild.

**Noch offen.** DID 952 `HydraulicMatrix` (51 Byte, drei Zeilen zu 17) und
1549 `HydraulicMatrixConfiguration` (97 Byte) sind der eigentliche
Hydraulikplan. Zwei Anlagen liegen vor (252-A A06 mit direktem Kreis und
integriertem Speicher; 150-A A13 mit Trennpuffer, gemischtem Kreis 2 und
Frischwasserstation, aus Diskussion #22): in 952 ist die erste Zeile bei
beiden `01 08`, die zweite beginnt mit `01 05` beziehungsweise `01 01` und
trägt dahinter unterschiedliche Bytes; in 1549 haben beide dasselbe Gerüst
(Byte 33 `01` plus Code, Byte 46 `01 1x 20`, ab Byte 51 eine Zählung mit
Einträgen: einmal `01 03 03`, einmal `04 03 00 09 00 09 00 09`), die Anlage
mit Puffer zusätzlich `02 00 01 07` am Anfang. Zum Festnageln fehlen weitere
Konfigurationen, etwa 250-A ohne Puffer mit zwei Kreisen oder eine Vitodens.
Das Profil zeigt beide roh. Wer eine anders konfigurierte Anlage hat
(Puffer, mehrere Heizkreise, Kühlung, Monoblock): Scan und Sondierung helfen,
die Matrix über mehrere Anlagen zu vergleichen und die Modelltabelle
(Herstellnummer → Modell) zu füllen. Welche zwei Dateien das sind und was vor
dem Hochladen zu beachten ist, steht unter
[Bekannte Anlagen](anlagen.md#meine-anlage-fehlt).
