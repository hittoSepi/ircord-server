# IRCord — Teknologiavertailut

Viisi avointa valintaa ja niiden trade-offit.

---

## 1. Serialization: Protobuf vs FlatBuffers

### Protobuf

**Plussat:**
- Erittäin laaja ekosysteemi, dokumentaatio ja tooling (grpcurl, protoc plugins, language support)
- Pienempi wire size (~20-30% pienempi kuin FlatBuffers tyypillisesti)
- Schema evolution on helppoa: kenttien lisäys/poisto backward-compatible
- `proto3` syntaksi on suoraviivainen, generoidut C++ classit ovat selkeitä
- Arena allocation tukee poolattua muistinhallintaa
- Debuggaus helppoa: `TextFormat::PrintToString()` → luettava output

**Miinukset:**
- Vaatii aina deserialisoinnin ennen käyttöä — kopioi dataa
- Deserialisointi hitaampaa: ~350 ns/op vs FlatBuffersin ~80 ns/op benchmarkeissa
- Generoidut headerit voivat olla isoja, kasvattaa compile-aikoja
- `libprotobuf` on ~2-4 MB linkattuna

### FlatBuffers

**Plussat:**
- Zero-copy deserialisointi: lue suoraan bufferista ilman kopiointia (~0.09 µs)
- Erittäin matala latenssi — suunniteltu peleihin ja reaaliaikasovelluksiin
- Pienempi runtime-kirjasto (header-only, ei libprotobuf-tyyppistä riippuvuutta)
- Tukee mutable buffereita — voi muokata paikallaan ilman uudelleenserialisointia
- Nopea compile: header-only, ei massiivisia generoituja tiedostoja

**Miinukset:**
- API on rajoitetumpi ja kömpelömpi kuin Protobuf
- Schema evolution vaatii enemmän tarkkuutta (kenttien järjestys merkitsee)
- Vähemmän tooling-tukea ja community-resursseja
- Wire size tyypillisesti isompi kuin Protobuf (ei pakkaa yhtä aggressiivisesti)
- Builder-pattern serialisointiin on verbosempi

### Suositus: **Protobuf**

Chat-sovelluksessa viestit ovat pieniä (<1 KB) ja niitä tulee korkeintaan
satoja sekunnissa. Protobufin deserialisointiviive (~350 ns) on täysin
merkityksetön tässä kontekstissa. Schema evolution ja tooling-etu on
paljon arvokkaampi kun protokolla kehittyy iteratiivisesti.

FlatBuffers olisi perusteltu jos tämä olisi peliserveri jossa käsitellään
kymmeniätuhansia paketteja sekunnissa.

---

## 2. TUI: ncurses vs notcurses

### ncurses

**Plussat:**
- De facto standardi — löytyy käytännössä jokaisesta Unix-järjestelmästä
- Erittäin vakaa, battle-tested vuodesta 1993
- Valtava määrä dokumentaatiota, tutoriaaleja ja esimerkkejä
- Kevyt, minimaaliset riippuvuudet
- Toimii kaikkialla: vanhasta VT100:sta moderniin terminaaliin
- SSH-yhteensopivuus käytännössä taattu

**Miinukset:**
- API on C:tä vuodelta 1993 — ei RAII:ta, manuaalista muistinhallintaa
- Värituki rajoittuu 256 väriin (ei true color ilman hackeja)
- Ei nativia Unicode-tukea — `ncursesw` erillinen
- Ei kuvatukea (ei sixel, ei kitty graphics)
- Input-käsittely on kömpelöä (escape-sekvenssien parsinta manuaalista)
- Ei thread-safe (globaali tila)

### notcurses

**Plussat:**
- Moderni C/C++ API, thread-safe suunnittelusta asti
- 24-bit true color natiivisti
- Kuvat terminaalissa: sixel, kitty graphics protocol, framebuffer
- Unicode natiivisti (UTF-8 fundamental unit)
- Unambiguous keyboard protocol -tuki (ei escape-sekvenssiambiguiteettia)
- Animaatiot, fade-efektit, alpha blending
- "Assumes the maximum, steps down" -filosofia → moderni default

**Miinukset:**
- Paljon vähemmän dokumentaatiota ja esimerkkejä kuin ncursesista
- Ei yhtä portaabeli — vaatii modernin terminaalin (ei toimi kaikkialla SSH:n yli)
- Riippuu edelleen terminfosta (ncursesin terminfo-kirjastosta)
- Vähemmän käyttäjiä → vähemmän Stack Overflow -vastauksia ja apua
- API muuttuu nopeammin — voi breikkailla päivityksissä
- Windows-tuki on rajallinen/kokeellinen

### Suositus: **notcurses**

IRCord on uusi projekti moderneille terminaaleille — ei tarvitse tukea
VT100:aa tai 90-luvun SSH-setuppia. True color, Unicode ja
thread-safety ovat oikeasti hyödyllisiä: link preview -boxit näyttävät
paremmilta, voice status -indikaattorit voi värikoodata, ja
audio-threadin UI-päivitykset ovat turvallisia.

Vaihtoehto: aloita notcursesilla, mutta pidä rendering-logiikka
abstraktiokerroksen takana (`IRenderer`) niin backend on vaihdettavissa
jos ilmenee portability-ongelmia.

---

## 3. Voice Transport: libdatachannel vs oma UDP + libsrtp

### libdatachannel

**Plussat:**
- Valmis ICE + DTLS-SRTP stack out-of-the-box
- NAT traversal (libjuice integroitu) toimii ilman erillistä työtä
- Kevyt: ~20 MB vs libWebRTC:n ~600 MB
- WebRTC-yhteensopiva — jos joskus haluaa browser-clientin, se toimii
- SCTP data channels bonuksena (file transfer voicen rinnalla)
- vcpkg-tuki, CMake-integraatio suoraviivainen
- Sama kehittäjä ylläpitää libjuicea — tiivis integraatio

**Miinukset:**
- Ei bandwidth estimationia eikä bitrate-adaptaatiota
- Ei audio processingia (echo cancellation, noise suppression) — pitää hoitaa itse
- Vähemmän kontrollia transportin yksityiskohtiin
- WebRTC signaling -malli voi tuntua ylimitoitetulta kaveriporukalle
- Riippuvuus ulkoisesta kirjastosta joka voi muuttua

### Oma UDP + libsrtp

**Plussat:**
- Täysi kontrolli pakettiformaattiin ja protokollaan
- Minimaalinen overhead — ei WebRTC-frameworkin ylimääräisiä kerroksia
- Helpompi debugata verkkoliikennettä kun tietää tarkalleen mitä lähetetään
- Voi optimoida juuri tämän use casen tarpeisiin
- Vähemmän kokonaiskoodia jos ei tarvitse kaikkia WebRTC-featureita
- Opettavainen: ymmärrät koko stackin

**Miinukset:**
- NAT traversal pitää toteuttaa itse (STUN/TURN, ICE) — tämä on PALJON työtä
- DTLS handshake itse: OpenSSL DTLS API on tuskallinen
- Jitter buffer pitää kirjoittaa itse
- Packet loss concealment itse
- Bugit NAT traversalissa → voice ei toimi kaverilla jonka reititin on tiukka
- Kuukausia lisätyötä ennen kuin voice on yhtä luotettava

### Suositus: **libdatachannel**

NAT traversal on se juttu joka ratkaisee. Kaveriporukassa on
väistämättä joku CG-NAT:n takana tai yliopiston verkossa jossa UDP
on rajoitettu. libjuicen valmis ICE-implementaatio säästää viikkoja
debuggausta. Voit aina lisätä oman jitter bufferin ja audio
processoingin päälle.

---

## 4. Auth: SRP vs Identity Key Challenge

### SRP (Secure Remote Password)

**Plussat:**
- Zero-knowledge: serveri ei koskaan näe salasanaa edes rekisteröinnissä
- Serverin tietokanta ei sisällä password-equivalenttia dataa — murto ei paljasta salasanoja
- Mutual authentication: molemmat osapuolet todistavat identiteettinsä
- Tuttu UX: käyttäjä kirjautuu nick + password
- Ei vaadi käyttäjän hallitsevan avainpareja
- Kestää man-in-the-middle -hyökkäykset

**Miinukset:**
- Monimutkainen protokolla — helppo tehdä implementaatiovirheitä
- Vähemmän valmiita C++ kirjastoja (OpenSSL:ssä on SRP, mutta API on hankala)
- Salasana on silti heikoin lenkki — brute force mahdollinen offline
- Ei anna forward secrecyä itsessään (tarvitsee lisäkerroksen)
- Ei sido autentikaatiota Signal Protocoliin suoraan

### Identity Key Challenge (asymmetric)

**Plussat:**
- Luonnollinen integraatio Signal Protocoliin — sama Identity Key
- Ei salasanaa = ei salasanan brute forcea
- Yksinkertainen: serveri lähettää challengen, client signaa Identity Keyllä
- Forward secrecy jos challenge sisältää ephemeral keyn
- Trust model on yhtenäinen: "luotat kaverin identity keyhin" = luotat kaveriin
- Helppo toteuttaa libsodiumilla (Ed25519 sign/verify)

**Miinukset:**
- Key management -taakka käyttäjälle: identity key pitää varmuuskopioida
- Ei "unohdin salasanani" -flowta — key katoaa = tili menetetty
- Uusi laite vaatii key transferin (QR-koodi, manuaalinen export)
- UX on vieraampi ei-teknisille käyttäjille
- Jos identity key vuotaa, hyökkääjä voi impersonoida pysyvästi

### Suositus: **Identity Key Challenge**

Kaveriporukalle jossa kaikki ovat teknisiä tyyppejä, password on
turha indirectio. Identity key on jo olemassa Signal Protocolista —
miksi keksiä toinen autentikaatiomekanismi? Rekisteröinti = generoi
keypair, valitse nick, upload public key serverille. Login = signaa
serverin challenge. Yksinkertainen ja kryptografisesti puhdas.

Bonus: jos joku menettää keyn, kaverit voivat "vouch" uudelle keylle
(web of trust -tyyliin) ja serverin admin voi resetoida.

Recovery-flow: identity key export/import tiedostona, suojattu
passphrasella (Argon2id + XChaCha20-Poly1305).

---

## 5. Transport: TCP + TLS 1.3 vs QUIC

### TCP + TLS 1.3

**Plussat:**
- Yksinkertainen ja hyvin ymmärretty — OpenSSL/BoringSSL integraatio suoraviivainen
- Toimii kaikkialla — ei palomuuri-ongelmia (TCP 443 on aina auki)
- Debuggaus helppoa: Wireshark, tcpdump ymmärtävät TCP:tä
- Ei tarvitse UDP-tukea serverin hostauympäristössä
- Pienempi koodipohjaan — vähemmän abstraktiota tarvitaan
- TLS 1.3 on jo nopea: 1-RTT handshake, 0-RTT resumption

**Miinukset:**
- Head-of-line blocking: yksi hävinnyt paketti blokkaa kaikki striimit
- Reconnect vaatii uuden TCP handshaken + TLS handshaken
- Ei multiplexointia ilman ylimääräistä frameworkia (HTTP/2 tai oma)
- Connection migration ei toimi (IP/portti vaihtuu → yhteys katkeaa)

### QUIC

**Plussat:**
- Ei head-of-line blockingia — eri striimit ovat riippumattomia
- 0-RTT reconnect tunnetulle serverille — laptop lid-close → lid-open → instant
- Connection migration: IP-osoite vaihtuu (wifi → 4G) → yhteys säilyy
- Sisäänrakennettu TLS 1.3 — ei erillistä handshakea
- Multiplexoidut striimit natiivisti — eri kanavien viestit eivät blokkaa toisiaan
- Google raportoi 3.6% latenssiparannuksen Haussa ja 15.3% YouTube-bufferinnissa

**Miinukset:**
- Monimutkaisempi implementoida ja debugata
- UDP:tä blokkaavat jotkin yrityspalo­muurit ja hotelliverkot
- C++ kirjastot vähemmän kypsiä: quiche (Cloudflare), msquic (Microsoft), ngtcp2
- Congestion control on monimutkaisempi kuin TCP:ssä
- Wireshark-debuggaus vaatii SSLKEYLOGFILE-setupin QUIC:lle
- Fallback TCP:hen tarvitaan silti backup-planina

### Suositus: **Aloita TCP + TLS 1.3, lisää QUIC Phase 5:ssä**

TCP+TLS toimii heti, kaikkialla, ilman yllätyksiä. Kaveriporukan
käytössä head-of-line blocking on harvoin ongelma (viestit ovat
pieniä ja harvoja verrattuna web-trafficiin). QUIC:n todellinen etu
(connection migration, 0-RTT) on kiva-to-have mutta ei kriittinen
MVP:ssä.

Arkkitehtuuri kuitenkin kannattaa suunnitella niin että transport on
abstraktoitu (`ITransport` → `TcpTransport`, `QuicTransport`) jotta
QUIC on lisättävissä myöhemmin ilman massiivista refaktoria.

---

## Yhteenveto

| Valinta | Suositus | Perustelu |
|---|---|---|
| Serialization | **Protobuf** | Parempi tooling, schema evolution, riittävän nopea |
| TUI | **notcurses** | Moderni, true color, thread-safe, Unicode |
| Voice transport | **libdatachannel** | Valmis NAT traversal, säästää viikkoja |
| Auth | **Identity Key** | Luonnollinen Signal-integraatio, ei salasanoja |
| Transport | **TCP+TLS → QUIC** | Aloita yksinkertaisella, päivitä myöhemmin |

### Kriittiset abstraktiot jotka kannattaa tehdä alusta asti

```cpp
// Nämä interfacet mahdollistavat teknologiavaihdot myöhemmin
class ITransport;        // TCP nyt, QUIC myöhemmin
class IRenderer;         // notcurses nyt, mahdollinen GUI myöhemmin
class IVoiceTransport;   // libdatachannel nyt, vaihtoehto myöhemmin
class ISerializer;       // Protobuf nyt, ehkä FlatBuffers hot pathiin
```
