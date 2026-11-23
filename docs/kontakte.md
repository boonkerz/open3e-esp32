# Kontakteingänge und Schaltausgänge (Klingel, Türkontakt, SG-Ready-Relais)

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

## Schaltausgänge

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

## Verkabelung

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

## Warum es eine Abfallzeit gibt

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
