# SenseCAP Claude Monitor

Diese Firmware zeigt das Claude-Restkontingent auf dem SenseCAP Indicator D1 mit ESP32-S3, WLAN und 480 × 480 Display. Die Ansicht folgt der zuletzt gelieferten Bildvorlage: schwarzer Hintergrund, eigenes orangefarbenes Pixelmaskottchen, „Usage“ und Uhr, zwei dunkle Karten mit großen Prozentwerten, Restkontingentbalken und Reset-Countdowns. Der lokale Account-QR und die direkte Token-Erneuerung laufen auf dem Gerät.

Die orange Akzentfarbe ist auf Wunsch `#D97757`; auch der orange Stützpunkt der Kontingentbalken verwendet diesen Wert.

Die gesamte Benutzeroberfläche ist auf Englisch: Dashboard, Start-/Fehlerstatus und lokale Account-Einrichtung im Handybrowser. Die Sprache der externen Claude-Anmeldeseite wird vom Anbieter gesteuert.

Die UI verwendet **LVGL 8.3.1** mit ESP-IDF 5.1.1. Version `0.4.0-reference-ui` vereinheitlicht die kleinen Beschriftungen auf Lato Bold (23 px), reduziert die Werte auf 48 px und verwendet kompakte 432 × 132 px Karten mit 5 px Eckenradius. Die Flächen starten mit vollständig zurückgesetzten LVGL-Stilen; der Hintergrund ist exakt schwarz. Der Titel verwendet weiterhin Bree Serif.

Ein erfolgreicher Build oder Flash belegt keinen erfolgreichen Konto-Login. Login, Kontingentvergleich, Neustart und mindestens ein echter automatischer Refresh müssen am Gerät abgeschlossen werden.

## Verifizierter Stand am 26 Oktober 2026

`0.6.1-lively-clawd` ist ausschließlich in die Anwendungspartition geflasht und per Hash bestätigt. Neustart und HTTP 200 mit beiden bekannten Kontingenten unter der gespeicherten Anmeldung sind bestätigt. Die lebendigere Headeranimation besteht alle nativen Sanitizer-Szenen und 28 Unit-Tests; die unabhängige Prüfung bestätigt zusätzlich 13 relevante Szenen und sichtbare Maskottchen-Pixel innerhalb x28–83 und y24–64. Die Darstellung auf dem physischen Panel ist noch visuell zu bestätigen.

`0.6.0-footer-words` ist ausschließlich in die Anwendungspartition geflasht und per Hash bestätigt. Neustart und HTTP 200 mit beiden bekannten Kontingenten unter der gespeicherten Anmeldung sind bestätigt. Die Zeitangabe ohne Sekunden, wechselnde Wörter, Statuspriorität und der Refresh-Tap im Footer bestehen die nativen Sanitizer-Prüfungen; alle 28 Unit-Tests bestehen. Der native Vergleich mit der Gerätekonfiguration für Usage und Setup besteht ebenfalls. Die physische Lesbarkeit der neuen 12-px-Zeitangabe ist noch visuell zu bestätigen.

`0.5.3-english-ui` ist ausschließlich in die Anwendungspartition geflasht und per Hash bestätigt. Starttest, gespeicherte Anmeldung und HTTP 200 mit beiden bekannten Kontingenten sind bestätigt. Geräte- und lokale Setup-Texte sind Englisch; die zugehörigen Statusabgleiche und nativen Tests wurden gemeinsam angepasst. Alle 26 Unit-Tests und die nativen ASan-/UBSan-Szenen bestehen; die unabhängige Prüfung meldet keine wesentlichen Befunde.

`0.5.2-mascot-motion` ist ausschließlich in die Anwendungspartition geflasht und per Hash bestätigt. Der Starttest bestätigt die neue Version und HTTP 200 mit beiden bekannten Kontingenten unter der gespeicherten Anmeldung. Native Sanitizer-Prüfungen belegen begrenzte Bewegung, Rückkehr ohne Drift, unveränderte Inhalte außerhalb des Maskottchens sowie Ruhe bei Fehlern und Refresh. Alle 26 Unit-Tests bestehen. Die physische Bewegung bleibt visuell zu prüfen.

`0.5.1-boot-face` ist ausschließlich in die Anwendungspartition geflasht und per Hash bestätigt. Der Neustart meldet die neue Version, WLAN, SNTP und HTTP 200 mit beiden bekannten Kontingenten unter der gespeicherten Anmeldung. Boot-Gesicht, langsamer Start, Setup-/Fehlerabbruch und bestehender Tap-Refresh bestehen die nativen ASan-/UBSan-Prüfungen; alle 26 Unit-Tests bestehen. Die tatsächliche Bootanimation und die kleinere Schrift am Panel sind noch visuell zu bestätigen.

Zuvor wurde `0.5.0-tap-wink` ausschließlich in die Anwendungspartition geflasht und per Hash bestätigt. Der Neustart hat die gespeicherte Anmeldung geladen und beide Kontingente mit HTTP 200 gelesen. Ein anschließender realer Tap auf die Statuszeile wurde angenommen und hat unmittelbar einen weiteren erfolgreichen Usage-Abruf ausgelöst. Der ESP-IDF-Build und alle 26 Unit-Tests bestehen; die native Fünf-Sekunden-Animation samt Touch-Ereignissen besteht ASan/UBSan. Die Animation selbst ist am Panel noch nicht visuell bestätigt. Die originale Seeed-Panelinitialisierung bleibt erhalten.

Der ESP-IDF-Build und 15 automatische Tests sind erfolgreich. Die erste Prüffirmware wurde über `/dev/cu.usbserial-110` geschrieben und gestartet. Das Nutzerfoto bestätigt die angemeldete Kontingentansicht. Das neue Layout wird zusätzlich direkt mit dem produktiven LVGL-Code in nativen 480 × 480 Bildern geprüft. Der vollständige automatische Refreshzyklus bleibt gesondert zu validieren.

Version `0.2.0-usage-display` wurde anschließend ausschließlich in die Anwendungspartition bei `0x10000` geschrieben und per Hash bestätigt. Der Neustart hat die vorhandene Anmeldung wiederverwendet und erfolgreich HTTP 200 mit bekannten Werten für beide Kontingente empfangen. Acht native Ansichten einschließlich Aktualisierung, Fehler und Verbindungsverlust bestehen die Sanitizer-Prüfung. Die Farbanpassung am echten Panel steht noch zur Sichtprüfung aus.

Version `0.3.0-refresh-blink` ergänzt das refreshgesteuerte Blinzeln. Anwendung und Hash wurden auf dem ESP geprüft; der Neustart hat mit gespeicherter Anmeldung erneut beide Kontingente mit HTTP 200 gelesen. Die native Blink-Regression besteht einschließlich Fehlerabbruch und schneller Antworten unter ASan/UBSan. Ein vollständiger echter Token-Ablauf bleibt weiterhin separat nachzuweisen.

Version `0.4.0-reference-ui` wurde nach der unabhängigen Nachprüfung ausschließlich in die Anwendungspartition geflasht; der Hash ist bestätigt. Der Starttest meldet die neue Version, WLAN, SNTP und HTTP 200 mit beiden bekannten Kontingenten unter der gespeicherten Anmeldung. Alle neun nativen Szenen bestehen ASan/UBSan, einschließlich sichtbarer abgeblendeter Stale-Balken, Wartezuständen und Blinzeln; alle 15 Unit-Tests sind erfolgreich. Ein neues Hardwarefoto zur Farb- und Lesbarkeitsprüfung steht aus.

Der Nutzer meldet auch mit `0.4.0` weiterhin falsche Blau-/Cyanfarben und schlechte Schrift. Die bisherigen Panelanpassungen sind damit keine bestätigte Lösung. Ein Hostvergleich mit den tatsächlichen LVGL-Geräteeinstellungen zeigt nur geringe Änderungen an gemischten Kantenpixeln; er reproduziert den Farbstich nicht. `python3 scripts/compare_device_config.py usage setup` legt die Vergleichsbilder getrennt unter `build/previews/device-config-*` ab.

## Temporärer Rohpixel-Test

`CONFIG_CLAUDE_DISPLAY_DIAGNOSTIC=y` baut einen eigenen Programmeinstieg ohne LVGL, WLAN, OAuth oder NVS-Anwendungscode. Der Test schreibt bekannte RGB565-Farbflächen, Graustufen und 1-Bit-Bitmaps in beide Bildpuffer und verwendet unverändert dieselbe Panelinitialisierung wie der Monitor. Er bleibt bis zum Zurückflashen sichtbar; in dieser Zeit werden keine Kontingente abgerufen. Das ist ein Diagnosebild, keine korrigierte Oberfläche. Ein Foto der beschrifteten Flächen entscheidet, ob die Fehler bereits ohne LVGL auftreten. Der Test protokolliert außerdem die ausschließlich lesenden Touch-Adressen-Proben `0x48` und `0x38` als Hinweis auf die Panelvariante.

```sh
bash scripts/idf.sh -B build-display-check -DCLAUDE_RAW_DISPLAY_BUILD=ON -DSDKCONFIG=.private/sdkconfig.display-check '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.display-check.defaults' build
```

Der normale Build bleibt separat unter `build/`. Das vor dem Test geflashte Monitor-Binary ist zusätzlich privat unter `.private/pre-display-check-monitor.bin` gesichert; es enthält noch die verworfene Panelanpassung. Zur Rückkehr nach dem Panelvergleich den neu gebauten Monitor unter `build/` ausschließlich bei `0x10000` schreiben; weder Flash löschen noch NVS überschreiben. Die Diagnose-Build-Option ist standardmäßig aus. Vier Hosttests prüfen Rohfarben, begrenzte Schreibzugriffe, identisches Rendern zweier Puffer und vollständige Farblabels; insgesamt bestehen 19 Unit-Tests.

`0.4.1-raw-display-check` ist mit bestätigtem Flash-Hash gestartet und meldet das eingereichte Rohpixelmuster. `0x48` antwortet, `0x38` nicht; das passt zur gewählten GX-Touch-Konfiguration. Das Gerätefoto IMG_1441 zeigt falsche Grundfarben, einen Gelbstich im Weiß und eine bunte statt neutrale Graurampe bereits ohne LVGL. Fehlende T/U/Y im Testbild waren zusätzlich ein behobener Fehler der diagnostischen Bitmap-Schrift.

`0.4.2` entfernt die experimentellen COLMOD-/BGR-Overrides vollständig. Diagnose und Monitor kompilieren und linken wieder ausschließlich die originale Panelinitialisierung des festgelegten Seeed-SDK mit `COLCTRL=08`, `MADCTL=10`, `COLMOD=60`. Diese Kombination berücksichtigt die Modulverdrahtung; die 16-Bit-Breite des ESP-Bildpuffers allein bestimmt den ST7701-Pixelmodus nicht. Die Farbwiedergabe dieses Vergleichs muss am Panel bestätigt werden.

Das anschließende Gerätefoto `IMG_1442.jpg` zeigt mit dieser Originalinitialisierung korrekt zugeordnete rote, grüne, blaue und Clay-Flächen sowie eine Graurampe ohne die vorherigen bunten Sprünge. Damit ist die Korrektur der Panelkonfiguration am Rohpixelbild visuell belegt; das Foto ist keine Farbkalibrierung. Die normale Kontingentansicht `0.4.2-vendor-panel` wurde danach mit bestätigtem Hash wiederhergestellt. Neustart, WLAN, SNTP und HTTP 200 mit beiden bekannten Kontingenten unter der gespeicherten Anmeldung sind bestätigt. Eine Aufnahme der wiederhergestellten LVGL-Kontingentansicht steht gesondert aus.

Die bisherige Firmware ist vollständig als `.private/preflash-esp32s3.bin` gesichert. Die neue Partitionstabelle stimmt bytegenau mit der ursprünglichen überein. Prüfprotokolle und Hashes liegen privat unter `.private/`; das Backup enthält möglicherweise Zugangsdaten und darf nicht veröffentlicht werden.

## Bauen und flashen

Der offizielle Seeed-SDK-Stand ist auf `77edb8d2b9a92fc67965c1b2d4a838f0d09a1800` festgelegt. `scripts/idf.sh` nutzt die auf diesem Rechner installierte ESP-IDF 5.1.1 und die vorhandene ESP32-S3-Toolchain. Bei einem anderen Rechner die Pfade im Skript beziehungsweise `IDF_PATH` und `IDF_PYTHON_ENV_PATH` anpassen.

```sh
bash scripts/fetch-sdk.sh
# Nur bei der ersten Einrichtung der lokalen Python-Kompatibilitätsschicht:
/Users/max/.espressif/python_env/idf5.1_py3.9_env/bin/python -m pip install --target .deps/python --no-deps 'ruamel.yaml==0.17.40' 'ruamel.yaml.clib==0.2.8'
bash scripts/idf.sh build
python3 -m unittest discover -s tests -v
bash scripts/idf.sh -p /dev/cu.usbserial-110 -b 115200 flash
/Users/max/.espressif/python_env/idf5.1_py3.9_env/bin/python scripts/boot_check.py
```

Vor dem Flashen die vorhandene Firmware privat sichern. Das Flashen schreibt Bootloader, Partitionstabelle und Anwendung; **kein `erase-flash` ausführen**. Bestehende NVS-Daten bleiben nur erhalten, wenn Lage und Größe der bisherigen NVS-Partition mit der verwendeten Partitionstabelle übereinstimmen. Der zweite USB-Anschluss mit „INDICATOR RP2040“ ist nicht das Flash-Ziel.

## WLAN

Die Anwendung versucht zuerst, die im ESP-WLAN-Treiber gespeicherte Konfiguration wiederzuverwenden. Fehlt sie, zeigt das Display „WLAN fehlt“. Es gibt keinen Access Point und kein WLAN-Menü.

Für eine lokale Provisionierung `python3 scripts/provision_wifi.py` in einem Terminal ausführen. Das Passwort wird verdeckt abgefragt; die Datei `main/provision.h` ist privat und von Git ausgeschlossen. Anschließend neu bauen und flashen. Firmware-Binaries mit eingebauten WLAN-Daten ebenfalls privat halten. Vorhandene gespeicherte WLAN-Daten haben Vorrang; ein Netzwechsel ist in diesem Prüfstand noch nicht implementiert.

## Konto und Abnahme

1. Nach WLAN-Verbindung und Zeitsynchronisation erscheint „Connect Claude“ mit einer zehn Minuten gültigen lokalen QR-URL. Handy und ESP müssen sich im selben erreichbaren WLAN befinden.
2. QR scannen, auf der lokalen Seite den Claude-Login öffnen und anmelden. Den vollständigen ausgegebenen Code einschließlich `#state` zurück in die lokale Seite kopieren. Zugangsdaten gehören weder ins Chatfenster noch ins Repository.
3. Der ESP tauscht den Code direkt aus und schließt die Setup-Seite nach erfolgreichem Token-Austausch. Danach prüft er den ersten Usage-Abruf. Beide Restprozente und Reset-Zeitpunkte mit der Claude-Anzeige vergleichen. „Unknown“ ist kein bestätigter Wert.
4. Nach Stromtrennung muss das Gerät ohne Rechner und Handy mit seiner gespeicherten Anmeldung starten und erneut Usage lesen können.
5. Bis zum echten Access-Token-Ablauf laufen lassen. Die Firmware versucht 60 Sekunden vorher automatisch zu erneuern. Erst die Meldung `OAuth refresh succeeded` zusammen mit einem anschließenden erfolgreichen Usage-Abruf belegt den Refreshpfad. Keinen Ablauf durch geänderte Systemzeit vortäuschen.
6. WLAN-Ausfall, Rate-Limit und widerrufene Anmeldung getrennt prüfen. Temporäre Netzfehler dürfen gespeicherte Anmeldung nicht löschen. `invalid_grant` führt zur erneuten Anmeldung.

Tokens werden als einzelnes NVS-Blob im Namespace `claude_probe` gespeichert. Ein rotierter Refresh-Token bleibt bei Speicherfehlern im RAM und wird vor weiteren Requests erneut gespeichert. Stromverlust zwischen serverseitiger Rotation und lokaler Speicherung kann dennoch eine erneute Anmeldung erforderlich machen. Dieser Prüfstand aktiviert keine irreversible eFuse- oder Flash-Verschlüsselung. Bei physischem Zugriff ist der Token auslesbar; nach Ende des Tests die geräteeigene Anmeldung widerrufen.

Die Account-Seite läuft über lokales HTTP mit zufälliger temporärer Capability; Anthropic-Requests verwenden HTTPS mit Zertifikatsprüfung. WLAN und ein vorhandenes privates Flash-Backup entsprechend vertraulich behandeln. Tokens werden nicht über die Webseite ausgegeben und nicht protokolliert.

## Anzeige und verbleibende Hardwareprüfung

Der Usage Screen bleibt sichtbar und aktualisiert seine Karten an Ort und Stelle. Fehler erscheinen in der Statuszeile; alte Daten bleiben gekennzeichnet sichtbar. Nach WLAN-Wiederverbindung wird sofort neu abgerufen. Das neue Foto ersetzt die frühere orange Außenfläche durch Schwarz. Farben der Restkontingentbalken folgen weiterhin dem verbleibenden Anteil; unbekannte Werte werden nicht als frei dargestellt.

Unten rechts steht die Zeit seit der letzten erfolgreichen Messung in kleiner, gedämpfter Schrift: zunächst „Just now“, danach etwa „1 minute ago“ oder „2 minutes ago“, bei älteren Daten ganze Stunden oder Tage. Sekunden werden nicht angezeigt. In der Mitte wechseln ungefähr alle drei Sekunden kurze Clawd-Wörter wie „Pondering…“ und „Mulling…“. Dieser dekorative Ablauf beobachtet keine laufenden Claude-Prompts und stellt keinen echten Arbeitsfortschritt dar. Reale Aktualisierungs-, Warte-, Fehler-, Offline-, Stale- und Unknown-Meldungen haben Vorrang.

Der gesamte Footer behält seinen unsichtbaren 432 × 51 px Touch-Bereich. Ein Tippen fordert über den vorhandenen Netzwerk-Worker einen Usage-Abruf an; wiederholte Eingaben während einer vorgemerkten Anfrage, eines laufenden Abrufs oder der Animation werden verworfen. Der automatische Fünf-Minuten-Abruf bleibt erhalten. Manuelle Abrufe umgehen weder HTTP-429-Wartezeiten noch Token-Refresh-Backoff. Die Touch-Achsen folgen dem offiziellen SenseCAP-LVGL-Port; ein realer Tap auf die Statuszeile hat auf dem Gerät einen erfolgreichen Abruf ausgelöst.

Ein logischer Refresh-Zyklus zeigt einmal für insgesamt fünf Sekunden eine große Clay-Fläche (`#D97757`) mit zwei dunklen rechteckigen Augen. Die Fläche blendet 300 ms ein, beide Augen blinzeln zwischen 1550 und 1840 ms, das linke zwinkert zwischen 2850 und 3180 ms; ab 4650 ms blendet die Ansicht bis 5000 ms zurück. Die Uhr bleibt an ihrer Position über der Fläche sichtbar und erhält darauf dunkle Schrift. Schnelle Antworten aktualisieren die verdeckten Karten, während die Animation ausläuft. Bei längerem HTTP-Abruf erscheinen nach fünf Sekunden wieder die Karten mit „Updating“; es gibt keine Animationsschleife. Token-POST, Usage-GET und ein 401-Retry teilen einen Zyklus. Fehler, WLAN-Ausfall und QR-Anmeldung brechen die Animation sofort ab; vor der ersten Nutzungsmessung wird keine Setup-Ansicht verdeckt.

Beim Start erscheint dasselbe große Gesicht mit einer kleinen Fortschrittszeile unter den Augen. Der erste Blink-/Zwinkerablauf bleibt mindestens ungefähr fünf Sekunden sichtbar; dauert die Verbindung länger, bleiben die Augen offen und der tatsächliche Startstatus sichtbar. Nach der ersten Nutzungsmessung folgt das Dashboard. Anmeldung und Fehler unterbrechen das Startgesicht. Startstatus und zentrierte Footerzeile verwenden Lato Bold mit 16 px; die Zeitangabe unten rechts verwendet 12 px. Der 432 × 51 px Touch-Bereich des Footers bleibt erhalten.

Das kleine Clawd-Maskottchen oben links läuft und wippt in einem festen Vier-Sekunden-Zyklus: 1,2 Sekunden Ruhe, 2,4 Sekunden Bewegung in 100-ms-Schritten und 0,4 Sekunden Ruhe. Es bewegt sich bis zu zwölf Pixel seitlich sowie vier Pixel nach oben und unten. Drei Pixel hohe Fußschritte und gegenläufige Armbewegungen machen den Charakter deutlich lebendiger. Die Bewegung bleibt im Headerbereich; Uhr, Titel und Karten behalten ihre Position. Bei Start-/Refresh-Gesicht, Setup, Aktualisierung, Fehlern und veralteten Daten ruht das kleine Maskottchen. Ein angenommener Refresh-Tap stellt sofort die Ruhepose wieder her. Die Bewegung ist dekorativ und zeigt keine laufenden Claude-Prompts an.

Die ursprüngliche Änderung auf `COLMOD=50` und die zusätzliche BGR-Umstellung waren nicht ausreichend durch die Modulverdrahtung begründet und wurden entfernt. Der [ST7701S-Datensatz, Tabelle auf Seite 73 und COLCTRL auf Seite 276](https://cdn-shop.adafruit.com/product-files/5799/ST7701%20Datasheet.pdf) beschreibt unterschiedliche Eingangspad-Zuordnungen für RGB565 und RGB666 mit `MDT=1`. Die Firmware übernimmt wieder die originale Kombination des Seeed-Moduls. Der LVGL-Port startet weiterhin beide Bildpuffer schwarz. Native Vorschauen belegen die physische Farbwiedergabe nicht.

`python3 scripts/render_display.py --sanitize` erzeugt native Vorschauen mit klar als Beispieldaten verwendeten Werten unter `build/previews/` und prüft sie mit Adress- und Undefined-Behavior-Sanitizern. Die Szene `face-refresh` prüft zusätzlich echte simulierte LVGL-Zeigerereignisse und die einmalige große Augenanimation. `boot`, `boot-slow` und `boot-interrupt` prüfen den Startablauf, Statuswechsel, langsame Antworten sowie Abbruch durch Fehler und Anmeldung. Diese Bilder verändern keine Werte auf dem ESP. `scripts/boot_check.py --no-reset` liest gefilterte Diagnosen, ohne den Monitor für einen Touch-Test neu zu starten.

OAuth ist eine inoffizielle Anbindung. Der Prüfstand verwendet die in der installierten Claude-CLI 2.1.236 enthaltenen aktuellen Hosts: `claude.com/cai/oauth/authorize` für die Kontoanmeldung sowie `platform.claude.com` für Code-Callback und Token-Austausch. Die angeforderten Scopes sind `user:profile user:inference`; der tatsächliche Konto-Flow muss noch validiert werden. Es werden keine lokalen Claude-Code-Zugangsdaten importiert oder geteilt.

## Quellen und Lizenzen

- [Seeed SenseCAP ESP32 SDK](https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32): Board- und Displayansteuerung; Apache-2.0, Lizenzhinweise des übernommenen LVGL-Ports bleiben erhalten. Die Abhängigkeiten enthalten ihre eigenen Lizenzdateien.
- [Claude Login Referenz](https://github.com/grll/claude-code-login/blob/main/index.ts): PKCE und manueller Code-Austausch; ihre älteren Hostnamen werden nicht unverändert übernommen.
- [Claude Quota Referenz](https://github.com/yehjxraymond/claude-quota/blob/master/claude-quota.ts): Usage-Felder und Refresh-Anfrage.
- [Monitor Learnings](https://github.com/mathd/claude-codex-monitor/blob/main/LEARNINGS.md): Erfahrungswerte zu OAuth und Refresh.

Es wurden keine Clawdmeter-Sprites übernommen.

Die eingebetteten Bitmap-Schriften basieren auf Bree Serif und Lato Bold. Die SIL-OFL-Lizenzen und Copyright-Hinweise liegen unter `licenses/`. Die konvertierten Ausschnitte heißen `usage_heading_font`, `usage_values_font`, `usage_ui_font`, `usage_status_font` und `usage_meta_font`.
`bash scripts/generate_fonts.sh` erzeugt sie bei Bedarf erneut mit festgelegten Quell-Hashes und `lv_font_conv` 1.5.3; normale Firmware-Builds benötigen keinen Font-Download und kein Node.js.
