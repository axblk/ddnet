# Bericht: Umsetzung des eBPF-Flutschutzes

Branch `quic-webtransport`, drei Commits auf `fcedc044d`. Alles gebaut und getestet auf
Ubuntu 26.04, clang 21.1.8, rustc 1.93.1, libbpf 1.6.3.

| | Commit | Umfang |
|---|---|---|
| 1 | `8ce6e8ac6` server: Bound the work a spoofed address can still cost | 5 Dateien, +109/−8 |
| 2 | `074b696c0` xdp: Add the standalone flood filter | 8 Dateien, +1712/−1 |
| 3 | `c199fd35c` server: Derive tokens the XDP filter can verify | 15 Dateien, +930/−36 |

## 1. Fixes

**B1 — unbegrenzte Dekompression für etablierte 0.6-Slots.** `CNetServer::Recv` hatte drei
von vier Pfaden gedeckelt: 0.7 vergleicht vorher den Header-Token, ein Absender ohne Slot
verbraucht `sv_preconn_decompress_per_second`, aber ein 0.6-Paket von einer Adresse mit Slot
wurde bedingungslos dekomprimiert. Da der 0.6-Token hinter der Kompression liegt, lässt sich
über so ein Paket vorher nichts sagen — wer die Adresse eines verbundenen Spielers spooft,
kaufte damit beliebig viele Huffman-Dekodierungen, das Teuerste, was ein einzelnes Paket
kosten kann. Neu: `sv_conn_decompress_per_second` (250) pro Slot, beim Neubelegen des Slots
zurückgesetzt, damit niemand ein aufgebrauchtes Budget erbt.

**F2 — zwei SHA256 pro Connless-Paket.** `GetGlobalToken()` hing von nichts Paketspezifischem
ab und wurde trotzdem pro Paket neu abgeleitet. Wird jetzt einmal beim Öffnen berechnet.

**B2 — QUIC-Sessions vor jeder Per-IP-Prüfung.** `MAX_SESSIONS` (1024) zählte Handshakes und
fertige Verbindungen gemeinsam, `IDLE_TIMEOUT` ist 30 s, und `sv_max_clients_per_ip` greift
erst im `CONNECTED`-Event — also nach dem Handshake, den es begrenzen soll. Rund 34 begonnene
und dann liegengelassene Handshakes pro Sekunde reichten, um alle Slots dauerhaft zu belegen.
Neu: eigenes Limit für halboffene Verbindungen (128), höchstens 16 gleichzeitige Verbindungen
pro Quelladresse, und ein Handshake, der nicht vorankommt, wird nach 5 s geschlossen statt 30 s
zu liegen. Die Zählung läuft inkrementell, weil sie auf einem Pfad sitzt, dessen Rate ein
Angreifer bestimmt.

## 2. Der eigenständige Filter

`src/xdp/`, gebaut mit `-DEBPF=ON`. Ein XDP-Programm plus der Dienst, der es lädt.

**Warum eigenständig.** Pro Interface kann nur ein XDP-Programm hängen. Ein Host mit mehreren
DDNet-Instanzen hätte sonst die erste gewinnen lassen und der Rest wäre nicht gestartet.
Nebeneffekte: kein Serverprozess braucht `CAP_BPF`, und der Filter überlebt einen
Serverneustart.

**Zustandslos.** Was durchgelassen wird, wird durchgelassen, weil das Paket einen Wert trägt,
den nur dieser Host ausgegeben haben kann — den 0.7-Security-Token oder eine QUIC Connection
ID, beide per SipHash-2-4 aus dem Schlüssel des Dienstes abgeleitet. Eine Off-Path-Quelle kann
keines von beidem erzeugen. Ein Client, der die Adresse wechselt, funktioniert weiter, weil
seine Identität im Paket steckt und nicht in einer Tabelle.

**Was sich nicht verifizieren lässt, wird begrenzt.** 0.6 hat seinen Token hinter der
Kompression, 0.6-Connless gar keinen. Beide bekommen ein Budget, verteilt über Quell-Prefixe:
wer wenige Prefixe hält, leert nur deren Eimer, ein Spieler aus einem unbeteiligten Prefix
findet einen vollen. QUIC-Initials werden nach ihrer Token-Länge getrennt — das Feld ist
fälschbar, kauft also ein eigenes Budget und nie einen Freifahrtschein. Master-Challenges
werden überhaupt nicht budgetiert; sie zu verlieren kostet keinen Durchsatz, sondern den Platz
in der Serverliste.

**Scharfschaltung pro Port.** Ein Port verwirft erst, nachdem er verifizierten Verkehr getragen
hat. Die Portliste ist ein Vertrag; ohne diese Sperre würde ein versehentlich eingetragener
Port oder einer, dessen Server ohne `EBPF` gebaut wurde, seinen gesamten Verkehr verlieren
statt nur ungeschützt zu sein.

**Eine bewusste Abweichung vom Rust-Klassifikator.** Ohne Verbindungstabelle gibt es niemanden
zu fragen, ob eine Quelle ein etablierter Legacy-Peer ist. Ein Paket mit gesetztem
QUIC-Fixed-Bit ohne gültige Connection ID fällt deshalb auf die Legacy-Prüfung durch, statt
verworfen zu werden — 0.6 setzt dieses Bit bei jedem Resend.

## 3. Im Server

Hinter `-DEBPF=ON`; der Default-Build ist bitgenau der vorherige.

- `CNetServer::GetToken()` leitet mit SipHash-2-4 ab **und nimmt den Port auf**. Die alte
  SHA256-Ableitung ließ ihn weg — der Kommentar im Code sagte das selbst —, also teilten sich
  alle Clients hinter einer Adresse einen Token, und wer seinen eigenen kannte, konnte Pakete
  für die Verbindungen seiner Nachbarn fälschen.
- Eigener QUIC-`ConnectionIdGenerator`. Quinn gibt über die Lebenszeit einer Verbindung neue
  IDs aus, innerhalb des Endpoints, wo C++ sie nicht sieht; eine tabellenbasierte Lösung
  müsste jede vor ihrer ersten Benutzung kennen. Der Generator liest seinen Schlüssel über eine
  gemeinsame Zelle, damit eine Rotation ihn erreicht, ohne den Endpoint neu aufzubauen, und
  behält die vorherige Generation gültig. Layout nach QUIC-LB, Schlüsselgeneration in den
  ersten drei Bits.
- `sv_ebpf_key` (Default `/run/ddnet-xdp/key`). Ohne lesbaren Schlüssel bleibt die alte
  Ableitung — der Port wird dann nie scharf, der Server ist ungeschützt statt abgeschnitten.

## 4. Was geprüft wurde

| Prüfung | Ergebnis |
|---|---|
| C++-Testsuite | 390 Tests grün, davon 5 neue für `CEbpfKey` |
| Rust-Testsuite | 23 grün, davon 4 neue für den CID-Generator |
| `ddnet-xdp-test` | 16 SipHash-Referenzvektoren + 24 Klassifikator-Vektoren + 28 Prüfungen der Token-Eimer grün |
| Build `EBPF=OFF` | unverändert grün |
| Build `EBPF=ON` | Server, Dienst und BPF-Objekt grün |
| `fix_style.py`, Header-Guards, Standard-Header, ungenutzte Header, Config-Variablen, alphabetische Ordnung, absolute Includes | alle grün |

Zwei Prüfungen sind die tragenden:

**SipHash gegen die Spezifikation.** Alle 16 Referenzvektoren aus dem SipHash-Papier, geprüft
für die C-Implementierung (`ddnet-xdp-test`) und die Rust-Implementierung (`cid::tests`)
getrennt.

**Die Ableitung über die Sprachgrenze.** `ebpf_key_test` pinnt die Tokens, die der Server für
konkrete Adressen ableitet, gegen Werte, die mit der C-Implementierung des Filters erzeugt
wurden. Eine Abweichung von einem Byte im Aufbau der kanonischen Eingabe würde jedes
verifizierte Paket verwerfen — dieser Test lässt sie stattdessen den Build brechen.

## 5. Stand der Prüfung

Der Filter lädt, und der Weg dahin ist der eigentliche Befund dieses Abschnitts.

```
the verifier accepted the program
processed 493237 insns (limit 1000000)
```

Die Zahl, die hier vorher stand — 135 416 — war echt, aber sie gehörte einem Stand,
den es nicht mehr gab. Ab `Let 0.6 prove itself, by not compressing and by being
remembered` hat der Verifier das Programm abgelehnt, und die fünf Commits danach
sind gegen einen Verifier geschrieben worden, der sie nie gesehen hat. Aufgefallen
ist das erst, als das Labor den Filter zum ersten Mal an ein Interface hängen
wollte. Sechs Commits lang stand im Repository, er lade.

Die Ursache war nicht die Menge an Arbeit, sondern ihre Form: eingebettet sind die
dreizehn Auswege des Klassifikators dreizehn Zustände, und der Verifier verfolgt
jeden davon durch alles, was danach kommt — Verbindungstabelle, zwei Token-Buckets,
Zähler. Als Aufruf fällt der Rahmen beim Rücksprung weg und sie sind wieder einer.
Eine Zeile, und aus `E2BIG` wird die Hälfte des Budgets.

Ebenfalls widerlegt, und zwar durch Messung statt durch Vermutung: die ausgerollten
SipHash-Durchläufe. Eine Fassung mit einer Addition an ihrer Stelle überzieht das
Budget genauso, eine mit nur einer Epoche statt vier ebenfalls. Sie kosten nichts.

Bis der Filter überhaupt so weit lud, waren sieben Läufe nötig, und jeder hat etwas
gefunden, das weder der Compiler noch ein Offline-Test hätte finden können:

| Befund | Art |
|---|---|
| `makes pkt pointer be out of bounds` | Payload-Größe aus Zeigerdifferenz, obere Hälfte unbekannt |
| `infinite loop detected` | Schleifenzähler auf dem Stack, weil seine Adresse an den Map-Lookup ging |
| `R4 !read_ok` | Puffer ohne Initialisierer, Kopierschleife bricht am Paketende ab |
| `invalid access to packet, r=0` | Bereichsprüfung nach `bpf_xdp_adjust_tail` mit abgeleiteter statt konstanter Länge |
| **0.7-Verkehr wäre verworfen worden** | Protokollerkennung an Flag-Bits, die 0.6 und 0.7 verschieden belegen |
| **Offload hätte jeden neuen Spieler ausgesperrt** | antwortete auch auf nicht scharfen Ports |
| **0.7-Token-Request wurde verworfen** | er trägt per Definition keinen gültigen Token |
| **IPv6 mit Extension-Header lief vorbei** | Programm gab auf, wo der Socket weitermacht |
| Alles Unverifizierbare teilte ein Budget | ein 0.6-Flood ließ Serverinfo verhungern |
| Keine Ban-Liste im Filter | war im Entwurf, nie gebaut |
| Schlüssel nach zwei Rotationen zurückgezogen | tötet laufende Sitzungen, nicht Handshakes |
| Scharfschalten ohne Gegenstück | ein Port ohne Server dahinter verwirft alles |
| **Programm seit sechs Commits nicht mehr ladbar** | dreizehn eingebettete Auswege, jeder durch alles danach verfolgt |

Die sechs fett gesetzten wären erst im Betrieb aufgefallen, und vier davon als
Ausfall statt als Fehler. Der letzte hätte bedeutet, dass gar nichts läuft.

Die Lehre daraus steht in `scripts/xdp_lab/`: eine Zahl aus einem Lauf gilt für den
Stand, auf dem sie gemessen wurde, und für keinen danach.

## 6. Was weiterhin offen ist

Der Filter ist geprüft, **in Betrieb war er noch nicht**. Ungetestet bleibt alles,
was ein einzelnes Laden nicht zeigt:

- **Nie an ein Interface gehängt.** `bpf_xdp_attach`, der Rückfall von Treiber- auf
  generischen Modus, das Pinnen der Maps und damit `--stats`.
- **Server und Filter liefen nie zusammen.** Die Ableitung ist über Sprachgrenzen
  hinweg per Testvektor abgesichert, aber ein echter Client, der sich durch den
  Filter verbindet, ist der eigentliche Beweis.
- **Kein Lasttest, keine Messung.** Die Zahlen in §2 bleiben Rechnung und Literatur.
- **Kein Paketumbau auf echter Hardware.** `XDP_TX` ist per `BPF_PROG_RUN` byteweise
  geprüft, aber eine NIC, die das Paket wirklich hinausschickt, hat es nie gesehen.
- **Die Verbindungstabelle** ist nur synthetisch geprüft; LRU-Verdrängung und
  Verfall nach Leerlauf hat noch nichts angefasst.
- **`bans_save` ruft niemand auf.** Der Dienst liest die Datei, schreibt sie aber
  nicht — der Betreiber braucht einen periodischen Aufruf im Server.
- **`cargo fmt` und `cargo clippy`** liefen nie, die Pakete fehlten. Sie sind
  inzwischen in `setup-dev-ubuntu.sh`.

## 7. Vergleich mit den Firewallregeln aus `ddnet-setup.sh`

Die offiziellen Server schützen sich mit `iptables` (ddnet-scripts, `ddnet-setup.sh`,
Zeilen 45–55). Die Regeln, auf UDP bezogen:

| Regel in `ddnet-setup.sh` | Entsprechung im Filter |
|---|---|
| `NOTRACK` für alles auf UDP | Nicht nötig: XDP läuft vor Netfilter. Die Regel bleibt für den Rest sinnvoll und stört nicht. |
| `u32 38=gie3` / `38=fstd` (Serverinfo-Anfrage), Ausnahme für den Master `37.187.108.123` | Klasse `connless`, Budget `--serverinfo-pps`; Master über `-m` (Name oder Adresse) nie budgetiert. Der Filter fasst weiter: jedes Connless-Paket, nicht nur `gie3`/`fstd`. |
| Serverinfo: `hashlimit-above 100/s`, Burst 250, **je Zielport** | Vorhanden: `--serverinfo-port-pps`, Standard 100 mit Burst 250, dieselben Werte. Ein zweiter Eimer je Zielport und Budget, zusätzlich zur Verteilung über Quellpräfixe; ein Paket braucht ein Token aus beiden, und keiner der beiden Eimer wird belastet, wenn der andere ablehnt. |
| Serverinfo: `hashlimit-above 20/s`, Burst 100, **je Quell-IP** | Vorhanden, strenger: 2000 pps auf 256 Präfix-Eimer ≈ 8 pps je /24 bzw. /56, Burst 32. |
| `u32 32=TKEN` (0.6-Connect mit Token-Magie): `100/s`, Burst 100, je Zielport | Klasse `handshake` (`is_legacy_connect` prüft dieselben Bytes an derselben Stelle, dazu die 0.7-Token-Anfrage), Budget `--handshake-pps` 5000 über Präfixe, dazu `--handshake-port-pps`, Standard 100 mit Burst 100, wie in `ddnet-setup.sh`; mit `--offload-handshakes` erreicht der Server gar keinen. |
| Statische `DROP`-Einträge für einzelne Adressen | `--bans FILE`, Format von `bans_save`, wird bei Änderung neu gelesen. |

Was `ddnet-setup.sh` nicht kennt und der Filter zusätzlich tut: Tokens und
Verbindungs-IDs prüfen (verifizierter 0.6-, 0.7- und QUIC-Verkehr wird nie budgetiert),
fehlgeformte Pakete verwerfen, QUIC-Verbindungsaufbau eigens deckeln (`--newconn-pps`,
das Pendant zur `TKEN`-Regel für QUIC, und `--newconn-port-pps` je Zielport), per
`--conntrack` je 0.6-Verbindung ein eigenes Budget, und Ports erst scharf schalten, wenn
sie nachweislich einen DDNet-Server tragen.

**Ergebnis:** Die Stoßrichtung deckt sich, und die Deckelung ist jetzt gleich
geschnitten. `iptables` deckelt je Quell-IP *und* je Zielport; der Filter je Quellpräfix
*und* je Zielport. Für eine Serverinfo-Flut von vielen gefälschten Absendern heißt das
mit den Standardwerten: höchstens 100 pps auf einen einzelnen Port, gleich über wie
viele Präfixe sie verteilt ist, wo vorher bis zu 2000 pps ankamen. Je Quelle bleibt der
Filter strenger als `iptables` (≈ 8 pps je /24 statt 20/s je Adresse).

Um `ddnet-setup.sh` zu entsprechen, ist nichts mehr einzustellen: `--serverinfo-port-pps`
100 mit Burst 250 und `--handshake-port-pps` 100 mit Burst 100 sind die Standardwerte
und spiegeln die beiden `hashlimit`-Regeln je Zielport samt ihren Bursts.
`--legacy-port-pps` und `--newconn-port-pps` haben dort kein Gegenstück und stehen auf
0, also unbegrenzt; wer eine andere Rate als den Standard setzt, bekommt als Burst das
2,5-fache davon. Die Eimer je Port liegen wie die Präfix-Eimer je CPU, mit der Rate
durch die CPU-Zahl geteilt, damit beide Reihen mit derselben Näherung zu beurteilen
sind. Beides lässt sich außerdem stapeln: Was der Filter durchlässt, sieht `iptables`
weiterhin.

## 8. Was als Nächstes zu tun ist

1. **Die fehlenden Prüfwerkzeuge nachinstallieren**, dann die beiden ausstehenden
   Prüfungen laufen lassen:

   ```sh
   sudo apt install rustfmt rust-clippy
   cargo fmt -- --check && cargo clippy --features ddnet-engine-shared/quic
   ```

2. **Im Zählmodus an ein Interface hängen.** Das ist der erste Schritt, bei dem
   etwas schiefgehen kann, was `ddnet-xdp-verify` nicht sieht: das Anhängen selbst,
   der Rückfall auf den generischen Modus, das Pinnen.

   ```sh
   sudo ./build/ddnet-xdp -i "$(ip -o route get 1.1.1.1 | awk '{print $5}')" \
       -p 8303 -o build/ddnet_xdp_kern.o --key-group ddnet --count-only -v
   ```

3. **Einen Server dagegen starten und sich verbinden.** Das ist der eigentliche
   Beweis: erst wenn unter *pass* Verkehr in `0.7` oder `quic short` auftaucht,
   benutzen Server und Filter nachweislich dieselbe Ableitung.

   ```sh
   ./build/DDNet-Server sv_port 8303 sv_ebpf_key /run/ddnet-xdp/key
   sudo ./build/ddnet-xdp --stats
   ```

   Bleiben die verifizierten Klassen leer, stimmt etwas zwischen Schlüsseldatei und
   Server nicht — dann sagt das Serverlog, ob er den Schlüssel überhaupt gelesen hat.

4. **Scharf schalten**, indem `--count-only` wegfällt. Ports schalten sich selbst
   frei, sobald sie `--arm-after` verifizierte Pakete getragen haben.

5. **Lasttest** auf Basis von `scripts/run_quic_netem_test.sh`, um die Zahlen aus §2
   durch gemessene zu ersetzen.

Vor Schritt 2 zu klären: ob auf dem Zielhost bereits XDP läuft (Cilium o. ä.). Pro
Interface passt nur ein Programm; dann führt kein Weg an `libxdp` vorbei. In
Containern gehört der Dienst auf den Host, nicht ins `veth`.

## 9. Anhang: ursprünglich geplante Reihenfolge der Inbetriebnahme


1. `sudo ./setup-dev-ubuntu.sh --style`, dann `cargo fmt -- --check` und `cargo clippy`
   nachholen.
2. `cmake -B build -DEBPF=ON` und `ddnet-xdp` **mit `--count-only`** auf einer echten Maschine
   starten. Das lädt das Programm und beantwortet die Verifier-Frage, ohne dass ein Paket
   verloren gehen kann.
3. Zähler über echten Verkehr beobachten (`-v`). Erst wenn `0.7` und `quic short` unter *pass*
   erscheinen und `malformed` nicht ungewöhnlich hoch steht, ist die Klassifikation bestätigt.
4. `--count-only` weglassen. Ports schalten sich dann selbst scharf, sobald sie verifizierten
   Verkehr getragen haben.
5. Lasttest in einer Netzwerk-Namespace, Basis `scripts/run_quic_netem_test.sh`.

Nicht vor Schritt 2 klären: ob auf dem Zielhost bereits XDP läuft (Cilium o. ä.) — dann
braucht es den `libxdp`-Dispatcher, weil das Interface schon belegt ist. Und in Containern
gehört der Dienst auf den Host, nicht ins `veth`; die Schlüsseldatei wird hineingemountet.
