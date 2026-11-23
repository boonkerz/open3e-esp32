# Was (noch) nicht drin ist

- **RS485.** Hardware vorhanden, keine Anforderung.
- **DoIP** als Alternative zu CAN.
- **Schreiben über UDS-Service 0x77.** Gelesen und dekodiert wird er
  vollständig — auf einem Vitocharge-Bus läuft der gesamte Verkehr zwischen
  Backend-Gateway und Speicher darüber. Selbst schreiben kann die Firmware ihn
  nicht; das ginge nur gegen einen Gateway, der dieselben Datenpunkte alle zehn
  Sekunden neu setzt.
