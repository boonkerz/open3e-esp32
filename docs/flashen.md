# Ohne Toolchain: aus dem Browser flashen

<https://esp32can.thomas-peterson.de>

Board per USB anstecken, Knopf drücken. Der Browser schreibt über die
**Web Serial API** direkt in den Flash — keine ESP-IDF, kein Treiber, kein
Raspberry Pi. Das Board meldet sich über die native USB-Serial-JTAG-Schnittstelle
des ESP32-S3, die esptool-js unmittelbar anspricht.

Es braucht **Chrome** oder **Edge** ab 89, **Opera** ab 76, **Firefox** ab 151
oder **Chrome für Android** ab 151. Safari und iOS können es nicht — WebKit
lehnt Web Serial ab; die Seite sagt das, statt einen wirkungslosen Knopf zu
zeigen.

Die Seite liegt in [`site/`](https://github.com/boonkerz/open3e-esp32/blob/main/site/). GitHub Actions baut sie bei jedem Push,
der Webserver holt sich das Ergebnis selbst (siehe [`site/README.md`](https://github.com/boonkerz/open3e-esp32/blob/main/site/README.md));
die Binärdateien und das Manifest entstehen aus dem Build, damit die
Flash-Offsets nicht von `partitions.csv` abweichen können.

Wer selbst entwickelt, nimmt den Weg darunter.
