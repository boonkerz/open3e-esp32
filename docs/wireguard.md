# WireGuard

Das Gerät kann seine Netzanbindung durch einen WireGuard-Tunnel führen. Damit
ist die **komplette Fernwartung** möglich: Weboberfläche, Firmware-Update über
*Einstellungen → Wartung* und MQTT laufen dann von der Gegenseite aus, ohne
dass Portfreigaben am Router nötig wären. Eingerichtet wird das unter
*Einstellungen → Anbindung → WireGuard*.

Die WireGuard-Implementierung ist die ESPHome-Bibliothek
([`esphome/wireguard`](https://components.espressif.com/components/esphome/wireguard)).
Sie wird über `main/idf_component.yml` aus dem ESP Component Registry geholt;
`esphome/libsodium` wird dabei als Abhängigkeit mit aufgelöst.

> **Build-Detail:** In `sdkconfig.defaults` ist `CONFIG_LWIP_PPP_SUPPORT`
> aktiviert. Das schaltet `LWIP_ESP_NETIF_DATA` ein, wodurch esp_netif seinen
> Zeiger in `netif->client_data` ablegt statt in `netif->state`. Ohne diese
> Betriebsart hält der esp_netif-Callback den Zustand des rohen
> WireGuard-Netifs für einen `esp_netif_t` und stürzt beim Anlegen in
> `esp_netif_internal_dhcpc_cb` ab. PPP selbst wird nicht verwendet.
> Die Größe des Allowed-IP-Arrays (`CONFIG_WIREGUARD_MAX_SRC_IPS`) setzt
> `main/Kconfig.projbuild`, weil die Registry-Komponente kein eigenes
> Kconfig mitbringt.

## Routing

Die **Netzmaske** ist der Regelfall: sie legt eine verbundene Route für das
Tunnel-Subnetz an. Liegen Broker und fernwartender Client darin — etwa
Tunnel `10.6.0.2/24` und Client `10.6.0.1` —, laufen Weboberfläche, OTA und
MQTT allein darüber. Eine Standardroute ist dafür **nicht** nötig.

Die optionale **Standardroute** leitet zusätzlich alles, was nicht ins lokale
WLAN gehört, durch den Tunnel:

| Standardroute | Wirkung |
|---|---|
| **aus** (Vorgabe) | Nur das Tunnel-Subnetz geht durch den Tunnel. Broker und Fernzugriff müssen darin liegen. |
| an | Zusätzlich geht alles außerhalb des lokalen WLANs durch den Tunnel. Nötig, wenn der Client in einem anderen Netz hinter dem Server liegt (z. B. `192.168.1.x`) oder das Gerät beliebige entfernte Ziele erreichen soll; der Broker darf dann irgendwo im entfernten Netz liegen. |

Für die übliche Konfiguration mit Broker und Fernzugriff im Tunnel-Subnetz
kann die Standardroute aus bleiben. Das lokale WLAN bleibt in beiden Fällen
direkt erreichbar: lwIP wählt für Ziele im eigenen Subnetz weiterhin die
WLAN-Schnittstelle. Ein falsch konfigurierter oder ausgefallener Tunnel sperrt
das Gerät deshalb nicht aus — man kommt per LAN weiterhin auf die
Einrichtungsseite.

Entscheidend ist nicht, woher die Verbindung kommt, sondern welche Quell-IP
in der Antwort steht: Ein Client auf dem WireGuard-Server selbst (etwa Home
Assistant auf `192.168.1.10`) kommt zwar über den Tunnel herein, seine
Antwortadresse liegt aber außerhalb des Tunnel-Subnetzes. Dafür ist die
Standardroute nötig. Ein echter WireGuard-Client mit eigener Tunnel-Adresse
(`10.6.0.x`) braucht sie nicht.

Die Standardroute wird erst gesetzt, **nachdem der Handshake steht**, und fällt
zurück, sobald der Peer nicht mehr antwortet. Sonst würden NTP und jeder
andere ausgehende Verbindungsversuch in einem toten Tunnel verschwinden.

Für den Fernzugriff im Browser die **Tunnel-Adresse** des Geräts verwenden
(z. B. `http://10.6.0.2`), nicht `open3e.local` — mDNS wird nicht über den
Tunnel übertragen.

> **Das Web-UI hat keine eigene Anmeldung.** Wer den Tunnel oder das entfernte
> Netz erreicht, kann Einstellungen ändern und Firmware schreiben. Das entfernte
> Netz muss also mindestens so vertrauenswürdig sein wie das lokale WLAN;
> der WireGuard-Schlüssel ist die einzige Zugangskontrolle.

## Gegenstelle

Auf dem WireGuard-Server braucht es einen Peer für das Gerät. Beispiel für
eine `wg0.conf` auf einem Linux-Server mit `10.6.0.1/24`:

```ini
[Interface]
Address = 10.6.0.1/24
ListenPort = 51820
PrivateKey = <Server-privater-Schluessel>

[Peer]
# oeffentlicher Schluessel, den das Geraet im Web-UI anzeigt
PublicKey = <Geraet-public-key>
PresharedKey = <optional>
AllowedIPs = 10.6.0.2/32
```

Damit das Gerät den Server und dessen LAN erreicht, muss der Server
weiterleiten (und für Internetzugriff maskieren):

```sh
sysctl -w net.ipv4.ip_forward=1
iptables -A FORWARD -i wg0 -j ACCEPT
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
```

Die Tunnel-Adresse des Geräts ist die `AllowedIPs`-Angabe auf der
Gegenseite. Auf dem Gerät ist `0.0.0.0/0` als erlaubte Quelle eingetragen —
das ist das ESPHome-Verhalten und erlaubt auch eingehende Verbindungen aus
dem entfernten Netz. Die eigentliche Netzauswahl macht die Route, nicht diese
Liste.

Der fernwartende Client braucht seinerseits eine Route zum Tunnel-Subnetz,
also z. B. `AllowedIPs = 10.6.0.0/24` (oder nur `10.6.0.2/32`) in seiner
WireGuard-Konfiguration. Danach ist das Gerät unter `http://10.6.0.2`
erreichbar; OTA und alle Einstellungen laufen über dieselbe Adresse.

## Betrieb

- **Der Endpunkt darf ein Hostname sein**, wird aber über das WLAN-DNS
  aufgelöst. Ein Name, den nur der entfernte DNS kennt, funktioniert nicht —
  dann die IP eintragen. Aufgelöst wird beim Verbindungsaufbau, nicht bei
  jedem Handshake: Ändert sich die Server-IP während eines längeren Ausfalls,
  wird sie erst beim nächsten vollständigen Verbindungsaufbau (Neustart,
  WLAN-Reconnect oder Speichern der Einstellungen) neu gelesen.
- **Die Uhr muss stehen, bevor der erste Handshake läuft.** WireGuard signiert
  Zeitstempel (TAI64N); mit einer Uhr aus dem Jahr 1970 verwirft die
  Gegenseite den Handshake unter Umständen als Replay. Die Firmware wartet
  deshalb bis zu 20 Sekunden auf SNTP und versucht es danach trotzdem, damit
  eine Anlage ohne NTP nicht dauerhaft ausgesperrt ist.
- **Keepalive** (Vorgabe 25 s) hält die NAT-Zuordnung offen, wenn zwischen
  Gerät und Server ein Router steht.
- Der Zustand steht auf der Statusseite („WireGuard") und unter den
  Einstellungen: `aus`, `wartet`, `verbinde…`, `verbunden` mit dem Alter des
  letzten Handshakes, oder `Fehler` mit der Ursache.

## Verbindungsabbrüche

Die Firmware ist auf längere Ausfälle ausgelegt; ein Eingriff ist nicht nötig.

**WireGuard-Server zehn Minuten weg.** Die Bibliothek schickt weiter
Handshake-Versuche (alle 5 s). Sobald der letzte Schlüssel nach
`REJECT_AFTER_TIME` (180 s) verfällt, meldet sie die Schnittstelle als down:
der Zustand wechselt von `verbunden` auf `verbinde…`, nach weiteren 60 s auf
`Fehler: kein Handshake …`, und die Standardroute — falls aktiviert — fällt
auf das WLAN zurück. Das lokale WLAN und die Weboberfläche bleiben die ganze
Zeit erreichbar. Kommt der Server zurück, gelingt der nächste Handshake
innerhalb von fünf Sekunden; die Firmware setzt die Standardroute wieder und
startet den MQTT-Client einmal neu, damit er nicht in seinem Backoff hängen
bleibt. Home-Assistant-Discovery, Kontaktzustände und der Poller-Refresh
laufen dabei erneut.

**MQTT-Broker fünf Minuten weg, Tunnel steht.** Der Tunnel bleibt durch den
Keepalive (25 s) offen. esp-mqtt erkennt die tote Verbindung und versucht es
standardmäßig alle zehn Sekunden erneut. Nach dem Reconnect werden LWT
(`online`), Abonnements, Discovery, Kontaktzustände und der Status erneut
gesendet; der Poller wird sofort angestoßen. Messwerte aus der Ausfallzeit
werden nicht nachgeliefert — der nächste Poll liefert die aktuellen Werte.

**Neustart während eines Ausfalls.** Die Standardroute wird erst nach einem
erfolgreichen Handshake gesetzt. Ein Gerät, das ohne erreichbaren Server
startet, bleibt deshalb über das lokale WLAN konfigurierbar; MQTT versucht es
weiter und verbindet sich, sobald der Tunnel steht.

Mit **Standardroute aus** betrifft ein Tunnelausfall nur das Tunnel-Subnetz;
NTP, Internet und das lokale WLAN laufen ungestört weiter. Mit Standardroute
an kann der übrige ausgehende Verkehr für die Dauer bis zum Schlüsselverfall
(ca. drei Minuten) ebenfalls stocken — danach wird die Route zurückgesetzt.
Für Broker und Fernzugriff im Tunnel-Subnetz ist die Standardroute daher
nicht nötig.

## Grenzen

- **IPv4 only.** Das ist eine Eigenschaft der Bibliothek, nicht eine
  Einstellung.
- **Das Tunnel-Subnetz darf nicht mit dem lokalen WLAN kollidieren.** lwIP
  wählt die Schnittstelle anhand von Adresse und Netzmaske; liegt das lokale
  Netz z. B. ebenfalls in `10.6.0.0/24`, gewinnt die zuerst eingetragene
  Route und die Zuordnung wird mehrdeutig. Im Zweifel ein eigenes Subnetz
  wie `10.44.0.0/24` verwenden.
- **Ein Peer.** Es gibt keine Site-to-Site-Konfiguration mit mehreren
  Gegenstellen.
- **Software-Krypto.** ChaCha20-Poly1305 läuft auf der CPU; für MQTT, HTTP und
  ein Firmware-Update reicht das locker, für Durchsatzmessungen nicht.
- **Die Schlüssel liegen im NVS** und in einer Sicherung im Klartext. Wer
  Zugriff auf das Gerät oder die Sicherungsdatei hat, hat auch den Tunnel.
