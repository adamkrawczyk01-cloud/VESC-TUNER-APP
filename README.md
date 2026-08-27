# VESC Tuner — czarna skrzynka dla desek na VESC

Telemetria i diagnostyka dla onewheeli na VESC (Floatwheel ADV2, Refloat).
Trzy elementy: **Cardputer** nagrywa jazdę, **HUD na kask** pokazuje ją przed okiem,
**analizator webowy** pozwala rozbierać ją potem na czynniki pierwsze.

> **Read-only.** Żadne z urządzeń nie potrafi zmienić ustawienia w desce — w kodzie
> nie istnieje ani jedna komenda zapisu. Wysyłane są wyłącznie `GET_*`, `PING_CAN`
> i `FW_VERSION`, plus jeden zaszyty na stałe literał `faults` do odczytu rejestru
> błędów. Strojenie robi się ręcznie w VESC Tool.

## Credits — VESCape

Parts of the design here are borrowed, **as ideas**, from
[VESCape](https://github.com/vescape-app/vescape) (GPL-3.0). Their ADRs are unusually
good engineering documentation, and several of them solved problems this project
had already run into the hard way:

| their ADR | what it fixed here |
|---|---|
| 0024 debug recording + replay | the raw-BLE `.jsonl` capture — which then immediately exposed a packet-drop bug we had misdiagnosed twice |
| 0013 hot/cold split | we were requesting the heavier Refloat ALLDATA payload on every poll and starving that feed |
| 0011 / 0016 IR-compensated SoC | battery % diving on every hill |
| 0008 sanitizers mark, never delete | the web analyser used to null out "impossible" samples, which destroys evidence you later need |
| 0021 idle pause | long parked tails bloating the logs |

**No VESCape code was copied.** This repo carries no license file and is not
GPL-licensed, so taking code was never an option. What was reused is the *shape of
the Debug Recording line format*, reimplemented from its documented layout — with
the useful side effect that recordings are interchangeable both ways.
`web/jsonl.js` opens VESCape's own `replay-thor301.jsonl` fixture, and the
Cardputer writes the same format.

Their source is kept out of this repo entirely (see `.gitignore`), the same way the
Refloat firmware source is.

---

## Schemat

```
        ┌──────────────┐   BLE / NUS      ┌─────────────────────────────┐
        │  DESKA       │◄─────────────────│  M5Stack Cardputer          │
        │  VESC Express│   tylko GET_*    │  • trzyma link BLE          │
        │      ↓ CAN   │                  │  • GPS + zapis na SD        │
        │  ADV500      │                  │  • 11 ekranów, klawiatura   │
        └──────────────┘                  └──────────┬──────────────────┘
               ▲                                     │ ESP-NOW 30 Hz
               │ BLE (gdy nie ma                     ▼
               │  Cardputera)              ┌─────────────────────────────┐
               └───────────────────────────│  M5 AtomS3R — HUD na kask   │
                                           │  • ekranik 128×128          │
                                           │  • panel LED 16×8           │
                                           │  • 3 kropki statusu         │
                                           └─────────────────────────────┘
                         karta SD
                            │
                            ▼
                 ┌─────────────────────────┐
                 │  web/  — analizator PWA │
                 │  CSV · Float Control    │
                 │  · .jsonl (replay)      │
                 └─────────────────────────┘
```

**Dwa źródła danych dla HUD.** Gdy Cardputer nadaje — HUD słucha ESP-NOW. Gdy go nie ma —
łączy się z deską sam po BLE (wybór deski przyciskiem, zapamiętuje wybór). VESC Express
przyjmuje **jedno** połączenie BLE, więc HUD zwalnia łącze, gdy Cardputer wraca.

---

## Cardputer — co nagrywa

| plik | zawartość |
|---|---|
| `session_NNN.csv` | **87 kolumn**, ~12 Hz — prędkość, prądy, duty, temperatury, pitch/roll, ADC footpadów, IMU, cele BMS, GPS, znaczniki świeżości |
| `session_NNN_raw.jsonl` | **każdy surowy chunk BLE** (tx i rx) ze znacznikiem czasu — nagranie do odtworzenia |
| `session_NNN_faults.txt` | zrzuty rejestru błędów prosto z deski (prąd chwilowy, filtrowany, duty, rpm w chwili odcięcia) |
| `session_NNN_mcconf.bin` / `.json` | konfiguracja silnika przy połączeniu |
| `session_NNN_appconf.bin` | konfiguracja aplikacji |
| `session_NNN_refloat.bin` | **strojenie jazdy** (kp, kp2, ki, ATR, tiltback) — surowo, żeby log wiedział, na czym jechał |

Nagranie startuje samo: przy prędkości > 2 km/h albo gdy Refloat wejdzie w stan RUNNING.
Postój dłuższy niż 30 s wstrzymuje zapis (marker `auto_pause` w `.jsonl`), ruszenie wznawia
natychmiast.

**Klawisze:** `[1]–[9]`, `[0]` ekrany · `[M]` następny · `[R]` nagrywanie ·
`[K]` **marker kopnięcia** · `[F]` odczyt rejestru błędów · `[J]` nagrywanie surowe on/off ·
`[P]` skan BLE · `[C]` konfiguracja WiFi

**Ekrany:** RIDE · DETAIL · TRIP · **FAULT** · CELLS · BACKUP · BOARD · CONFIG · REVIEW ·
bateria Cardputera · WIFI

---

## HUD — co pokazuje

Strony przełączane jednym przyciskiem: **SPEED · BATTERY % · BATT V · CELL V · MOTOR °C ·
BATT °C · CTRL °C · DUTY · GPS**.

**Trzy kropki statusu na dole każdej strony:**

| kropka | kolory |
|---|---|
| źródło danych | 🟢 Cardputer ma deskę · 🟡 Cardputer bez deski · 🔵 HUD trzyma deskę sam · 🔴 nic |
| footpad lewy | 🟢 wciśnięty · 🔴 zwolniony |
| footpad prawy | 🟢 wciśnięty · 🔴 zwolniony |

**Panel LED 16×8** — cztery słupki: prędkość, duty, bateria, plus ikona alertu
(przegrzanie, nadprąd, niskie napięcie). Te same słupki animują się falą, gdy HUD szuka deski.
Bez intro przy starcie — jeśli słupki się ruszają, HUD czegoś szuka.

Automatyczny obrót ekranu i LED-ów o 180° (IMU), potrójne kliknięcie przy prędkości 0
wraca do wyboru deski.

---

## Analizator webowy (`web/`)

PWA bez frameworka. Otwiera trzy formaty:

- **`session_NNN.csv`** — natywny format Cardputera
- **CSV z Float Control** — mapowanie kolumn
- **`.jsonl`** — nagranie surowe, dekodowane z powrotem do pełnego zbioru danych

Ostatni jest najciekawszy: **odtwarza jazdę bez deski.** Chunki BLE przechodzą przez to samo
składanie ramek i te same parsery co na żywo, więc analizę można powtarzać w nieskończoność
na tych samych danych.

Sanitizer **znaczy** podejrzane próbki zamiast je kasować (`d.excluded`) — statystyki je
pomijają, wykresy pokazują surowe. Usuwanie dowodów raz kosztowało dzień śledztwa.

---

## Sprzęt

- **M5Stack Cardputer** — ESP32-S3, klawiatura, ekran, SD, GPS
- **M5 AtomS3R** — HUD na kask, ekranik ST7735S + 2× panel M5 Puzzle 8×8
- **Deska** — Floatwheel ADV2 / ADV500, Refloat, mostek VESC Express (BLE→CAN)
- **Bateria** 20S2P

Wgrywanie: `cardputer/flash.sh [katalog_szkicu] [port]` — flashuje przez esptool
z `--flash-size detect`, bo `arduino-cli --upload` zostawia nagłówek 4 MB przy tablicy
partycji 8 MB i urządzenie wpada w pętlę restartów.

---

## Diagnostyka — `data/boards/`

Repo zawiera pełne konfiguracje dwóch desek (ten sam sterownik ADV500, różne silniki):
jedna jeździ czysto, druga sporadycznie ucina `ABS_OVER_CURRENT` przy ~2 km/h. Katalog
trzyma XML-e obu, log BMS i dokument konsultacyjny po angielsku
(`data/boards/GAD/CONSULTATION_2026-08-17.md`) opisujący, co zostało wykluczone pomiarem,
a co pozostaje otwarte.

## Czego tu NIE ma

- **Zapisu do deski.** Whitelist parametrów i mechanizm sugestii z wcześniejszej wersji
  projektu nie są podłączone do żadnej ścieżki zapisu — ekran REVIEW tylko wyświetla.
- **Dekodowania strojenia Refloat.** `session_NNN_refloat.bin` jest zapisywany surowo;
  offsety wymagają sparowania blobu z eksportem XML, tak jak zrobiono dla limitów mcconf.
- **Relaya BLE do telefonu.** Był napisany, przeszedł kompilację, nigdy nie trafił na sprzęt
  — świadomie wstrzymany.
