# Tests

Der C-Port wird nicht nach Augenmaß bewertet, sondern gegen open3e gemessen:
Die Testvektoren entstehen, indem die **echte Python-Implementierung** läuft.

```sh
make setup      # einmalig
make fixtures   # Vektoren aus open3e erzeugen
make test
```

| Test | Prüft | Umfang |
|---|---|---|
| `test_codec` | Dekodieren == open3e | 4692 Vektoren über alle 1564 DIDs |
| `test_encode` | Kodieren == open3e, **und** dieselben Ablehnungen | 3990 + 332 |
| `test_flatten` | geflachtes MQTT == open3es `mqttdump()` | 23 395 Topic/Wert-Paare |
| `test_isotp` | Segmentierung, Flow Control, SN-Wrap, Frame-Verlust | bis 4095 Byte |
| `test_uds` | Antwort, Schweigen, negative Antwort, responsePending | 5 Szenarien |
| `test_em380` | E380-Dekodierung == E3onCAN | 560 Vektoren über 14 CAN-IDs |
| `test_contact` | Entprellung an Gleich- und Wechselspannung, Topic-Namen | 6 Signalformen |
| `test_knx` | KNXnet/IP-Telegramme byteweise gegen die Spezifikation | 8 Frame-Arten, 14 Adressfälle |
| `test_topic` | Topic-Expansion der Weboberfläche == `mqtt_pub_topic()` | 18 Fälle, 3 an der Anlage geprüft |

`make lint` prüft zusätzlich Includes und modulübergreifende Symbole der
Firmware-Quellen, die sich ohne ESP-IDF nicht übersetzen lassen.

`test_uds` sichert eine Eigenschaft, die einmal gebrochen war: **eine Anfrage,
die nicht oder negativ beantwortet wird, darf keinen Erfolg melden.** Requests
liefen früher über eine FreeRTOS-Queue an eine CAN-Task; `xQueueSend` kopiert
per Wert, also füllte die Task das Ergebnis in ihrer eigenen Kopie aus, während
jeder Aufrufer seine eigene, null-initialisierte zurücklas — und ein genullter
`uds_result_t` liest sich als `UDS_OK`. Jede Anfrage schien zu gelingen, der
Bus-Scan „fand" an jeder Adresse ein Gerät und hinter fast jedem DID einen
Datenpunkt. Die Task und die Queue sind ersatzlos entfallen: der Austausch
läuft jetzt im aufrufenden Task unter einem Mutex, das Ergebnis ist der
Rückgabewert.

Zwei Details, die beim Portieren leicht schiefgehen und deshalb explizit
abgedeckt sind: `scale` ist beim **Dekodieren ein Divisor** (`O3EInt16` hat
Default 10.0), und vorzeichenlose 64-Bit-Zähler dürfen nicht über `int64_t`
laufen. Beide Fehler waren im ersten Wurf drin und sind von den Vektoren
gefunden worden.

## Test gegen einen simulierten E3

open3e testet selbst gegen
[virtualE3](https://github.com/philippoo66/vitualE3). Gleicher Aufbau, nur mit
echtem CAN statt `vcan0`:

```
virtualE3 (Docker, -c can0) ── USB-CAN-Adapter @250k ── CAN ── ESP32-S3
```

Erwartung: Die Firmware findet die ECUs `0x680` und `0x6a1` und dekodiert deren
Datenpunkte identisch zu open3e.
