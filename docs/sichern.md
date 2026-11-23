# Sichern und wiederherstellen

Unter *Einstellungen → Sichern und wiederherstellen*. Die Sicherung ist eine
einzelne JSON-Datei mit allem, was ein Gerät in denselben Zustand bringt:

| Enthalten | |
|---|---|
| WLAN | SSID, Passwort, Gerätename, Hotspot-Passwort |
| MQTT | Broker, Port, Zugangsdaten, Topics, Formatstring, HA-Discovery |
| WireGuard | Endpunkt, Tunnel-Adresse, Netzmaske, Keepalive, Schlüssel |
| System | Schreibfreigabe, E380, Zeitzone |
| Auswahl | Datenpunkte und Meter-Frames |
| Scan-Ergebnis | optional, abwählbar — der große, aber teuer zu wiederholende Teil |

**Die Datei enthält Passwörter.** Ohne sie wäre das Gerät nach dem
Wiederherstellen nicht erreichbar, eine Sicherung ohne WLAN-Passwort also
weitgehend nutzlos. Das gilt auch für den privaten WireGuard-Schlüssel und
den Preshared Key. Entsprechend aufbewahren.

Export und Import gehen beide streamend über die Dateien, ohne das Ergebnis im
Speicher zusammenzubauen — das Scan-Ergebnis allein kann mehrere hundert
Kilobyte groß sein. Nach dem Einspielen startet das Gerät neu.
