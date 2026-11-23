# KNX

Ein Kontakteingang kann beim Schalten zusätzlich ein **1-Bit-Telegramm
(DPT 1.001)** auf eine KNX-Gruppenadresse schreiben: geschlossen sendet 1,
offen sendet 0. Gedacht für die Klingel — so funktioniert sie auch dann, wenn
Broker oder Home Assistant gerade stehen.

Einschalten unter *Einstellungen → KNX*, die Gruppenadresse steht oben beim
jeweiligen Eingang. Beide Eingänge dürfen verschiedene Adressen bedienen.

Es wird ausschließlich **geschrieben**. Nichts wird gelesen, nichts abonniert,
und **KNX Secure wird nicht unterstützt** — eine gesicherte Installation
ignoriert, was hier gesendet wird.

## Tunnelling oder Routing

KNXnet/IP ist in beiden Fällen UDP; TCP käme erst mit KNX IP Secure.

| | Tunnelling | Routing |
|---|---|---|
| Ziel | ein IP-Interface, per Unicast | `224.0.23.12:3671`, Multicast |
| Zustand | Verbindung, Kanal-ID, Sequenznummern, ACK je Telegramm, Heartbeat alle 60 s | keiner |
| Voraussetzung | jedes IP-Interface kann es | ein IP-**Router**, der wirklich weiterleitet |
| Eigene Adresse | vom Gateway zugewiesen | selbst eintragen, z. B. `1.1.250` |

**Routing passt besser zu einer Klingel** — ein Ereignis am Tag, und keine
Verbindung, die dafür offengehalten werden müsste. Es setzt aber voraus, dass
ein Gerät Routing-Telegramme tatsächlich auf die Zweidrahtleitung legt. Ein
IP-*Interface* tut das meist nicht, **auch wenn es die Dienstfamilie Routing in
seiner Suchantwort meldet**. Ob es geht, sieht man am schnellsten so:

```sh
# 30 s auf der Routing-Gruppe mithören, während jemand einen Taster drückt
python3 - <<'EOF'
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 3671))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
             socket.inet_aton("224.0.23.12") + socket.inet_aton("0.0.0.0"))
while True:
    d, a = s.recvfrom(1024)
    if struct.unpack("!H", d[2:4])[0] == 0x0530:
        print(a[0], d.hex())
EOF
```

Kommt dabei nichts an, bleibt Tunnelling. Dessen Haken: ein Interface hat nur
eine Handvoll Tunnel-Kanäle, und ETS oder eine Visualisierung belegen sie —
das Gerät meldet dann `no free tunnel channel` im Zustand unter den
Einstellungen.

## Warum der Telegrammaufbau einen eigenen Test hat

Ein Telegramm mit falschem Längenbyte wird vom Gateway **kommentarlos
verworfen**: kein Fehler, kein Protokolleintrag, nichts auf dem Bus. Es gibt
nichts zu beobachten und nichts einzugrenzen. Deshalb liegt der ganze
Byte-Aufbau in `knx_frame.c` ohne Socket daneben, und `test_knx` prüft ihn
gegen die Layouts aus KNX 3/8/2 (Core), 3/8/4 (Tunnelling) und 3/6/3 (cEMI) —
Byte für Byte, einschließlich der Längenoktette und der Ablehnung von Frames,
die eine größere Länge behaupten als das Datagramm hat.

Das häufigste Missverständnis steckt im NPDU-Längenbyte: ein 1-Bit-Wert reitet
**im APCI-Oktett selbst** mit, die Länge ist also 1 und nicht 2. Setzt man dort
eine 2, meldet der Test vier fehlgeschlagene Vergleiche; auf dem Gerät hätte
man ein stummes Telegramm.
