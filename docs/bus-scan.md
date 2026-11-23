# Bus-Scan

| Modus | Umfang | Dauer |
|---|---|---|
| Schnellscan | nur die 1564 Datenpunkte der mitgelieferten Datenbank | ca. 1 Min. pro ECU |
| Vollscan | DID 256 bis 4000 | 10–20 Min. pro ECU |

Der Scan sucht ECUs auf den COB-IDs `0x680`–`0x6EF` über DID 256
(`BusIdentification`) und liest danach deren Datenpunkte. Das Zwischenergebnis
wird nach jeder ECU gespeichert, ein Neustart mitten im Vollscan verwirft also
nicht alles.

Findet der Scan keine ECU, stimmt fast immer eines davon nicht: Verkabelung,
Abschlusswiderstand, oder ein zweiter Master am Bus.

## Erkanntes Gerät

Beim ECU-Scan liest die Firmware **DID 256** (`BusIdentification`) und **DID 377**
(`ViessmannIdentificationNumber`) und zeigt daraus:

| Feld | Herkunft |
|---|---|
| Typ (`HPMUMASTER`, `EMCUMASTER`, …) | DID 256, `DeviceProperty` |
| Funktion | DID 256, `DeviceFunction` |
| Bustyp | DID 256, `BusType` |
| Software-/Hardware-Version | DID 256 |
| Seriennummer (VIN) | DID 256 |
| Viessmann-Identnummer | DID 377 |

DID 256 wird über den **echten Codec** dekodiert, nicht über feste Byte-Offsets.
Das ist nicht bloß sauberer: der Datensatz hat eine Längenvariante, und
handgerechnete Offsets würden auf einem Gerät, das die andere zurückliefert,
stillschweigend Unsinn produzieren.

## Gerätename und `{device}`

Jede ECU hat einen **frei wählbaren Namen**, editierbar in der Gerätetabelle.
Er füllt den Platzhalter `{device}` in den MQTT-Topics — exakt wie der
Schlüssel in open3es `devices.json`, dessen `dev_of_addr()` ebenfalls den
Benutzerschlüssel liefert und auf die Hex-Adresse zurückfällt.

Default ist deshalb `0x680`, nicht der Gerätetyp: ein Formatstring mit
`{device}` erzeugt so dieselben Topics wie vorher auf dem Pi. Vergebene Namen
überleben einen erneuten Scan.

## „nicht in DB"

Ein Datenpunkt landet nur dann in der Liste, wenn die ECU auf ihn mit einer
**positiven UDS-Antwort** geantwortet hat. Die Markierung „nicht in DB" heißt
also nicht, dass nichts zurückkam — sie heißt, dass open3e für diesen DID keine
Beschreibung mitliefert. Die Antwortlänge steht daneben als Beleg.

Solche Datenpunkte lassen sich trotzdem lesen und nach MQTT senden; der Wert
ist dann ein Hex-String, wie bei open3es Raw-Modus. **Schreiben ist für sie
gesperrt** — Bytes an eine Heizung zu schicken, deren Bedeutung man nicht kennt,
ist genau der Fehler, den es zu verhindern gilt.

Wer so einen DID identifiziert, kann ihn übrigens
[bei open3e beitragen](https://github.com/open3e/open3e/discussions).
