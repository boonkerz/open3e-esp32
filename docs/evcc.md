# evcc

evcc liest die Werte dieses Gateways über sein **MQTT-Plugin** und kann den
Speicher damit auch **steuern**. Unter *Einstellungen → evcc* erzeugt das Gerät
den fertigen Konfigurationsblock — aus der tatsächlichen Datenpunkt-Auswahl,
nicht aus einer Vorlage. Topics, Feldnamen und Zeitfenster stimmen also mit
dem überein, was die Firmware wirklich veröffentlicht.

| evcc | DID | Datenpunkt | Feld |
|---|---|---|---|
| Netz | 1603 | `PointOfCommonCouplingPower` | `ActivePower` |
| PV | 1690 | `ElectricalEnergySystemPhotovoltaicStatus` | `ActivePower cumulated` |
| Speicherleistung | 1836 | `ElectricalEnergyStorageCurrentPower` | — |
| Ladestand | 1664 | `ElectricalEnergyStorageStateOfCharge` | — |

evccs Vorzeichen: positiv ist einströmend — Netzbezug, PV-Erzeugung,
Speicher**entladung**. Die VX3 hält sich daran (DID 1836 beschreibt sich selbst
als „Battery Discharging (positive values)"), deshalb steht nirgends ein
`scale: -1`. Für PV ist 1690 die AC-Summe über die Strings; 1831 wären die
DC-Eingänge je String, die man addieren müsste und die um die
Wechselrichterverluste zu hoch lägen.

## Steuerung

`batterymode` ist ein Setter für Custom-Batteriemeter. evcc ruft ihn mit einer
Zahl auf, die die erzeugte Konfiguration auf die Befehle dieses Gateways
abbildet:

| evcc | bedeutet | Nutzlast |
|---|---|---|
| 1 `normal` | Hold beenden | `{"mode":"grid","addr":"0x6A1","stop":true}` |
| 2 `hold` | nicht entladen, PV-Laden erlaubt | `{"mode":"storage","addr":"0x6A1","storage":"nur laden","seconds":3600}` |
| 3 `charge` | aus dem Netz laden | `{"mode":"grid","addr":"0x6A1","watts":-2000,"seconds":3600}` |

Zwei Dinge daran sind nicht verhandelbar, beide an einer laufenden Anlage
nachgemessen:

**Die Adresse muss in der Nutzlast stehen.** Ohne `addr` geht der Befehl an die
Vorgabe `0x680`, und die trägt auf einer Anlage mit Wärmepumpe keinen der
beiden Datenpunkte — die liegen dort auf `0x6A1` (EMCUSLAVE). Der Hold meldet
sich dann als *aktiv*, schreibt nie, und ist nach fünf Fehlversuchen weg:

```
ohne addr:   active=True  writes=0  failures=3 → 5, dann tot
mit addr:    active=True  writes=2 → 4 → 6, failures=0
```

**Der `watchdog` ist nicht optional.** evcc schickt `batteryMode` nur bei
Änderung, und ein Hold endet hier nach höchstens einer Stunde von selbst — das
ist die Sicherheitseigenschaft, nicht ein Versehen. Der Watchdog schreibt alle
zehn Minuten nach; verschwindet evcc, greift die Frist trotzdem.

## Warum kein EEBUS

Die Frage kommt naheliegenderweise auf. Vier Gründe, alle geprüft:

1. **Über EEBUS kann evcc den Speicher nicht steuern.** Dessen
   EEBUS-Meter sind „EEBus grid meter" (Anwendungsfall MGCP, nur Netzzähler)
   und „EEBus consumer meter". Kein Batteriemeter, kein Ladestand, keine
   Steuerung.
2. **Über MQTT kann es das** — siehe oben.
3. **Von der Konfiguration nähme EEBUS nur den Netzzähler ab.** PV, Speicher,
   Ladestand und Steuerung blieben Handarbeit.
4. **Die Kosten träfen die knappste Ressource.** SHIP (mDNS, Selbstzertifikat
   mit SKI, TLS mit gegenseitiger Authentifizierung, Handshake) plus SPINE
   (Datenmodell, Discovery, Bindings) sind einige tausend Zeilen. Auf diesem
   Board sind im Tiefstand 70 KiB internes RAM frei, größter freier Block
   56 KiB; eine TLS-Verbindung mit Client-Zertifikatsprüfung braucht 30–45 KiB,
   und `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` zwingt mbedTLS' kleine
   Allokationen genau dorthin.
