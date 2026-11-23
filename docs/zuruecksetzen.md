# Zurücksetzen

Unter *Einstellungen → Zurücksetzen*, je ein Knopf pro Sache:

| Knopf | Löscht |
|---|---|
| Datenpunkt-Auswahl | `points.json` |
| Scan-Ergebnis und Auswahl | zusätzlich `system.json` |
| MQTT-Einstellungen | Broker, Topics, HA-Discovery |
| Alles außer WLAN | alle vier, dazu WireGuard, dann Neustart |
| WLAN vergessen | nur die Zugangsdaten, dann Setup-Hotspot |

Die WLAN-Daten bleiben bei allen außer dem letzten erhalten — ein
Zurücksetzen darf das Gerät nicht unerreichbar machen. Vor dem Löschen einer
Auswahl werden die Home-Assistant-Discovery-Topics geräumt, sonst blieben dort
Entitäten ohne Datenquelle stehen.
