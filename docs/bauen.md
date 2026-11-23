# Bauen und flashen

## Voraussetzungen

ESP-IDF **v6.x** wird gebraucht und ist nicht Teil dieses Repos. v6 ist keine
freie Wahl: die Firmware nutzt die TWAI-Node-API (`esp_twai.h`), und cJSON und
esp-mqtt sind dort keine eingebauten Komponenten mehr, sondern werden über den
Component Manager geholt (siehe `main/idf_component.yml`).

```sh
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh          # in jeder neuen Shell
```

## Datenbank und Web-UI erzeugen

Die open3e-Datenpunktdatenbank wird **nicht** mitgeliefert, sondern beim Bauen
aus einer festgepinnten open3e-Version erzeugt. Ein Update auf eine neuere
open3e-Version ist damit eine Zeile in `tools/fetch_open3e.py`.

```sh
make setup          # Python-venv anlegen, open3e holen
make db             # data/o3edb.bin erzeugen  (~1,6 MB)
```

Unter Windows ohne `make` und mit aktiviertem Python-Environment:

```powershell
python -m pip install udsoncan
python tools\fetch_open3e.py
python tools\gen_dpdb.py -o data\o3edb.bin
```

Die Ausgabe muss **14 E380 frames** melden. Fehlen die E3onCAN-Quellen oder
`udsoncan`, bricht die Erzeugung ab, statt eine Datenbank ohne E380 abzulegen.

`make storage` ist nur nötig, wenn man das Ergebnis ohne ESP-IDF ansehen will —
der Firmware-Build ruft `tools/build_fs.py` selbst auf.

`storage_image/` liegt bewusst **nicht** unter `build/` — das Verzeichnis
gehört CMake, und `idf.py set-target` verweigert seinen Fullclean, wenn dort
fremde Dateien liegen.

## Firmware bauen und flashen

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Änderungen an `web/` oder an `data/o3edb.bin` werden vom Build erkannt:
`storage_image/` wird neu zusammengestellt, zu `build/storage.bin` gepackt und
von `idf.py flash` mitgeschrieben. Ein separater `littlefs-flash`-Schritt ist
nicht nötig, und `make storage` von Hand auch nicht.

Für das Web-UI genügt also:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

Nur die **Datenbank** bleibt außen vor: `make db` muss man weiterhin selbst
aufrufen, wenn man auf eine neuere open3e-Version wechseln will. Das braucht
einen Netzwerkzugriff und die Python-Umgebung, was ein normaler Firmware-Build
nicht voraussetzen sollte.

Referenzgrößen eines sauberen Builds: App 1,12 MB (73 % der 4-MB-Partition
frei), DIRAM 121 KB von 342 KB.
