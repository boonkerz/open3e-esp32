# Erste Einrichtung

Nach dem Flashen erscheint ein WLAN namens **`Open3E-Setup-XXXX`**.

- Passwort: **`open3e-setup`** (im Einrichtungsdialog änderbar)
- Das Captive Portal öffnet die Seite automatisch; falls nicht:
  `http://192.168.4.1`

Nach dem Speichern startet das Gerät neu und ist unter `http://open3e.local`
erreichbar (bzw. unter dem eingestellten Gerätenamen).

**Wenn das WLAN nicht erreichbar ist** — Router getauscht, Passwort geändert —
spannt das Gerät nach 60 Sekunden den Einrichtungs-Hotspot *zusätzlich* wieder
auf und versucht im Hintergrund weiter, sich zu verbinden. Ein kurzer
Router-Neustart heilt sich damit von selbst, ein falsches Passwort bleibt
trotzdem korrigierbar, ohne das Gehäuse zu öffnen.
