# Aus dem Netz laden (Vitocharge VX3)

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

**Der Sollwert ist der Netzbezug, nicht die Ladeleistung.** Die Anlage hält
die Leistung am Netzanschluss auf den Wert, versorgt daraus zuerst das Haus
und gibt nur den Rest an den Speicher. Gemessen an einem Abend ohne PV:
`-2000` gehalten, 2011 W Bezug, 1550 W Hausverbrauch, 457 W in die
Batterie. Wer eine bestimmte Ladeleistung will, nimmt deshalb die zweite
Form, **„Speicher laden mit"**: die Firmware liest alle zehn Sekunden
`PointOfCommonCouplingPower` (DID 1603) und
`ElectricalEnergyStorageCurrentPower` (DID 1836) von derselben ECU und
setzt den Netzsollwert auf Hausverbrauch plus gewünschte Ladeleistung
nach, in Schritten von höchstens 1500 W, nie über 6000 W Bezug und nie als
Einspeisung. Lassen sich die beiden Werte gerade nicht lesen, bleibt der
Sollwert stehen. Stand der Wechselrichter in Bereitschaft, vergehen bis zu
zwei Minuten, bis die Ladung anläuft (DID 1552, Byte 1: `05` entlädt, `04`
lädt); gemessen: Ziel 2000 W, nach 2,5 Minuten 2012 bis 2033 W im Speicher
bei einem Sollwert von -2739 W und 720 W Hausverbrauch, drei Anpassungen.

## Bedienung

Einmalig: *Einstellungen* → **Schreiben freigeben**, und die ECU der
Speichereinheit heraussuchen — im Scan die mit `EMCUSLAVE`.

Von Hand: *Debug* → **Aus dem Netz laden** → Netzbezug negativ, Dauer in
Minuten, **Netzbezug halten**; oder in der zweiten Zeile die Ladeleistung und
**Laden**.

Aus einer Hausautomatisierung, für den Betrieb an einem dynamischen Tarif,
auf `<base>/cmnd`:

```json
{"mode": "grid", "addr": "0x6A1", "watts": -2000, "seconds": 1800}
{"mode": "grid", "addr": "0x6A1", "charge": 2000, "seconds": 1800}
{"mode": "grid", "stop": true}
```

`watts` ist der Netzsollwert, wie er geschrieben wird; `charge` die
Leistung in den Speicher, der Netzsollwert folgt. `/api/status` zeigt unter
`grid` beide Messwerte (`gridPower`, `storagePower`), das Ziel (`charge`),
den aktuellen Sollwert (`watts`) und die Zahl der Anpassungen.

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
