# Rohdaten für externe Integrationen

Für Software, die CAN-Frames selbst decodiert statt sich auf den Codec dieser
Firmware zu verlassen — etwa weil sie ihre eigene Datenpunkt-Datenbank
pflegt —, gibt es eine zweite, von der dekodierten Sicht oben komplett
unabhängige Schnittstelle auf Rohbyte-Ebene:

- **REST:** `GET /api/rawread` (bis zu 10 DIDs je Aufruf, Wartezeit je DID
  per `timeout=<ms>` wählbar, sonst 2,5 s) liefert die rohen
  UDS-Antwortbytes, `POST /api/rawwrite` (Service `0x2E`) schreibt sie.
- **MQTT:** die in der Einstellung `rawCanIds` (Komma-Liste) eingetragenen
  CAN-IDs werden roh auf `<baseTopic>/raw/<id-hex>` veröffentlicht —
  unabhängig von `points.json` und Auto-Discovery.

Beide Wege laufen komplett an der open3e-Datenbank vorbei — nichts davon wird
dekodiert oder auf `rw` geprüft. Vollständige Beschreibung, inklusive der
Scan-Delegation für einen entfernten Client:
[`docs/raw-gateway-api.md`](raw-gateway-api.md) — entstanden für die
Integration mit [ioBroker.e3oncan](https://github.com/MyHomeMyData/ioBroker.e3oncan),
der ersten Software, die diesen Weg nutzt.
