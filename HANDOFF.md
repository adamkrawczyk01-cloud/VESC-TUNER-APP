# HANDOFF — przekazanie kontekstu do nowej sesji

**Stan na: 2026-08-17.** Ten plik ma jeden cel: żeby kolejna sesja **nie powtórzyła błędów**
i **nie otwierała ponownie spraw już zamkniętych**. Czytaj w całości przed pierwszą odpowiedzią.

Repo: `/Users/adam/vesc-tuner` · gałąź `main` · język pracy: **polski**.
Pamięć trwała: `~/.claude/projects/-Users-adam-vesc-tuner/memory/` (4 pliki + `MEMORY.md`).

---

# CZĘŚĆ 0 — Jak pracować z tym użytkownikiem

Adam. Jeździ na onewheelach, buduje do nich własną telemetrię. Zna sprzęt, czyta ze zrozumieniem
i **wyłapuje błędy w rozumowaniu**. Kilka razy w poprzedniej sesji miał rację przeciwko mnie.

**Zasady wyprowadzone z realnych potknięć — łam je na własne ryzyko:**

1. **Zanim zdiagnozujesz cokolwiek z logów, ustal co użytkownik FIZYCZNIE robił.**
   Ogłosiłem raz „deska niezdatna do jazdy" na podstawie session_019. Okazało się, że celowo
   bujał deską najmocniej jak potrafił. To był stress test, nie awaria.

2. **„To wartość domyślna" / „jest taki commit" to NIE jest weryfikacja.**
   Weryfikacja = przeczytany kod, który faktycznie wykonuje się na tym sprzęcie. Trzy razy
   w jednej sesji oparłem wniosek na autorytecie i trzy razy było źle — raz dając fałszywe
   potwierdzenie, raz fałszywe wykluczenie. Patrz pamięć `weryfikuj-mechanizm-nie-autorytet`.

3. **Wycofanie bez dowodu jest tak samo złe jak twierdzenie bez dowodu.**
   Adam powiedział wprost: *„Nie uważam że jesteś w błędzie"*, gdy przesadziłem z odwoływaniem
   własnych ustaleń. Odróżniaj fakt techniczny od narracji — obalenie narracji nie obala pomiaru.

4. **Nigdy nie zmieniaj konfiguracji deski bez wyraźnej zgody.** Instrukcja padła dosłownie:
   *„pod żadnym pozorem nie zmieniaj konfiguracji deski"*. Dotyczy też narzędzi, które łączą się
   po BLE.

5. **Podawaj liczby, nie wrażenia.** Adam pyta „nie zgaduj, sprawdź, policz". Licz w Pythonie,
   pokazuj wzór i wynik. Jeśli czegoś nie wiesz — powiedz, że nie wiesz.

6. **To jest sprzęt, na którym człowiek jeździ 30 km/h.** Przy każdej propozycji zmiany podaj
   wpływ na bezpieczeństwo i na czucie jazdy. Nie certyfikuj sprawności deski — dawaj rozumowanie
   i zostawiaj decyzję jemu.

---

# CZĘŚĆ 1 — GŁÓWNY WĄTEK: kopnięcie i ABS_OVER_CURRENT w desce GAD

## 1.1 Konfiguracja fizyczna — zapamiętaj, bo to myliłem

| | **ADV2 „oryginalna"** | **GAD** |
|---|---|---|
| Rama | Floatwheel ADV2 | Floatwheel ADV2 |
| Sterownik | **ADV500** | **ADV500** (`adv500-lckx`) |
| Firmware | **6.05** (ConfigVersion 3) | **6.06** (ConfigVersion 4) |
| Silnik | **Cannoncore** (fabryczny) | **Sidewinder** (wymieniony) |
| Pakiet | Refloat 1.2.3 | Refloat 1.2.3 |
| Objaw | **żaden — działa** | kopnięcie przy starcie + ABS_OVER_CURRENT |

**To NIE są dwa różne sterowniki.** To ten sam typ sterownika z dwoma różnymi silnikami.
Kierowca ten sam: **106 kg** (w specyfikacji producenta).
Bateria 20S2P / 10 Ah / 720 Wh, spoczynkowo 77,38 V (3,86 V/celę). Przebieg GAD ~519 km.

Sesje na karcie SD rozróżnia sygnatura mcconf: **GAD = `2efd0142` (484 B)** (sesje 003–014, 019),
druga deska = `3f829cf7` (478 B) (sesje 015–018). To sygnatura **układu configu**, czyli funkcja
wersji firmware — **nie identyfikuje sprzętu**.

## 1.2 Trzy zdarzenia — jedyne twarde dane o awarii

| | 2026-08-09 | po tweakach Nica | **2026-08-17** |
|---|---|---|---|
| Current | 258,0 A | −236,0 A | **260,9 A** |
| Current filtered | 34,9 A | −14,3 A | **62,0 A** |
| **pik / filtr** | 7,4× | 16,5× | **4,2×** |
| Duty | 0,215 | **0,950** | 0,285 |
| RPM | −370,3 | **0,0** | **−588,3** |
| Voltage | 53,77 V | 65,68 V | 61,55 V |
| Temperatura | 31,4 °C | — | 26,3 °C |
| Cycles running | — | 99 | 28604 |

Limit: `l_abs_current_max = 225 A` (dotyczy wektora √(id²+iq²)).

**Napięcia w zrzutach to ZAPAD POD PIKIEM, nie stan baterii.** 77,38 − 61,55 = 15,83 V przy
~74 A prądu baterii ⇒ ~213 mΩ ⇒ ~21 mΩ/celę w 20S2P = norma. Ten sam błąd popełniłem przy
53,77 V z 9.08 („zapad do progu odcięcia") — to też był zapad, pakiet był zdrowy.

## 1.3 Gdzie awaria żyje — przeliczone na prędkość

PP=15, koło 0,28 m. `erpm/15/60 × π × 0,28 × 3,6 = km/h`

```
foc_hall_interp_erpm  250 =  0,9 km/h   ← powyżej: interpolacja hallów AKTYWNA
FAULTY                    =  1,3 / 0 / 2,1 km/h
foc_sl_erpm_start    1800 =  6,3 km/h   ← poniżej: pozycja WYŁĄCZNIE z hallów
foc_sl_erpm          2000 =  7,0 km/h   ← powyżej: obserwator
l_max_erpm          17000 = 59,8 km/h
```

**Wszystkie faulty poniżej tempa marszu, w oknie „halle + interpolacja".**
Mechanizm nie sięga powyżej 6,3 km/h — tam pozycję daje obserwator.
Konsekwencja dla bezpieczeństwa: odcięcie przy 2 km/h = zejście z deski, nie nosedive.

**⚠️ TRIPWIRE: jeśli w zrzucie pojawi się RPM > 2000 — cała ta analiza jest nieaktualna
i trzeba zacząć od nowa. Powiedz użytkownikowi, żeby przerwał jazdę.**

## 1.4 Co zostało wgrane 17.08 (aktualny stan deski)

| parametr | ścieżka w VESC Tool | było | jest |
|---|---|---|---|
| `foc_offsets_cal_mode` | FOC → Offsets | 0 | **1** (Calibrate on Boot) |
| `foc_sat_comp_mode` | FOC → Sensorless | 2 Lambda | **0 Disabled** |
| `m_hall_extra_samples` | FOC → Hall Sensors | 1 | **3** |

Zweryfikowane diffem: **zero zmian poza planem**. Offsety prądu przeliczyły się same
(−0,5 LSB) → dowód, że kalibracja przy starcie faktycznie działa.

**Efekt zmierzony:** pik/filtr **16,5× → 4,2×**, duty **0,95@0 erpm → 0,285@−588 erpm**.
Patologia „pełne napięcie na stojący silnik" zniknęła. Objaw został: **1 kopnięcie / ~15–30 prób**.

Ścieżki UI i wartości domyślne czytaj z:
`vedderb/vesc_tool` → `res/config/<wersja>/parameters_mcconf.xml`
(`valInt`/`valDouble` = default narzędzia, `enumNames` = nazwy opcji, sekcja `Grouping` = ścieżka).
**Uwaga:** default narzędzia ≠ default firmware. Firmware ma swoje w
`surfdado/bldc` → `motor/mcconf_default.h`. Przy sprzeczności **wygrywa firmware**.

---

# CZĘŚĆ 2 — ❌ HIPOTEZY OBALONE. NIE OTWIERAĆ PONOWNIE.

Każda z nich kosztowała godziny. Wszystkie mają twardy dowód.

### 2.1 Field weakening — OBALONE
Twierdziłem, że vanilla FW wstrzykuje 40 A w oś d przy duty 0,95 i 0 obr/min, bo brakuje
blokady prędkościowej. **Nieprawda.** `foc_fw_current_max = 40` jest na **OBU** deskach
(fabryczne), a blokada z commita `a8bc61a` **nie weszła do żadnej wydanej gałęzi** —
sprawdzone w `ADV_Vanilla_v605`, `v66_pinlock` i upstream. Wszystkie mają goły warunek na duty.
**Mój błąd:** znalazłem commit w historii repo i uznałem to za weryfikację, nie sprawdzając
gałęzi wydania.

### 2.2 Zepsuta tablica hallów — OBALONE, i to w drugą stronę
Liczyłem odstępy po **indeksie tablicy** i wychodziły absurdy (sektor 126°). Poprawnie:
kąt = `wartość/200 × 360°`, stany w kolejności **1-3-2-6-4-5**.

```
ADV2 (DZIAŁA):  52,2  55,8  61,2  64,8  66,6  59,4   → max odchyłka 7,8°
GAD  (KOPIE):   59,4  61,2  59,4  57,6  63,0  59,4   → max odchyłka 3,0°
```

**GAD ma RÓWNIEJSZĄ tablicę niż deska, która działa.** Potwierdza to `hall_analyze`
(7 przebiegów, 2,5–3,5°, zawsze 6 stanów). Halle są zdrowe. Wątek zamknięty.

### 2.3 „Elektryka silnika tłumaczy pik" — OBALONE
`di/dt = V/L` przy 61,55 V: **ADV2 692 A/ms, GAD 478 A/ms**.
Wyższa indukcyjność Sidewindera buduje prąd **wolniej**. Gdyby to był czysty transjent od źle
przyłożonego napięcia, bardziej narażona byłaby deska, która działa.

### 2.4 „Limit prądu za niski, podnieść 225 → 240" — OBALONE, i NIEBEZPIECZNE
`kt = 1,5 × 15 × flux`: ADV2 **0,574**, GAD **0,630 Nm/A**.
ADV2 @240 A = 137,7 Nm · **GAD @225 A = 141,8 Nm**.
**GAD ma o 2,9% WIĘCEJ momentu mimo niższego limitu.** Fabryka dobrała limit do silnika.

Dodatkowo: 2 z 3 faultów (258 i 261 A) przekroczyłyby też 240 A. A podniesienie limitu nie
zmniejsza prądu — pozwala mu płynąć **dłużej w złym kierunku**, czyli może zamienić odcięcie
napędu w mocniejsze kopnięcie. **Nie podnosić.**

### 2.5 `foc_hall_interp_erpm` 250 → 700/1800 — WYCOFANE
Sam to zaproponowałem, potem znalazłem w `foc_correct_hall()`, że **ten sam parametr jest
podłogą ogranicznika szybkości zmian kąta**:
```c
float angle_step = (fmaxf(rpm_abs_hall, conf_now->foc_hall_interp_erpm)/60.0) * 2*M_PI * dt * 1.5;
// Limit hall sensor rate of change. This will reduce current spikes in the current controllers
```
Podniesienie progu **wyłącza interpolację** (dobrze) ale **poluzowuje tłumik pików prądu** (źle).
Dwa przeciwstawne efekty. Do tego **obie deski mają 250**, więc nie ma wzorca. Odpuszczone.

### 2.6 Inne wykluczone
- **Bateria** — cele 3,703–3,717 V (Δ~10 mV), 25–27 °C, NO FAULT. Wzorowa.
- **Footpady** — 0% wartości pośrednich ADC; 14 z 15 największych szarpnięć przy OBU wciśniętych.
- **Temperatura** — 26,3 °C przy ostatnim faulcie.
- **Klik startowy** — `startup_click_current = 10` na obu deskach.
- **„Refloat zepsuty, 78% przestarzałych ALLDATA"** — artefakt pollingu Cardputera; deska,
  która działa, miała 83% stale i jeździła bez zarzutu.
- **Detekcja jako winowajca** — R 76,8→79,9 mΩ między czerwcem a sierpniem odpowiada
  **dokładnie** współczynnikowi temperaturowemu miedzi (0,393 %/°C), zgodność 0,4%.
- **Nasze zabawy z HUD/klonowaniem BLE** — sprawdzone, nie ruszyły configu deski.

### 2.7 Dwa błędy metodologiczne warte zapamiętania
- **„Sat comp = 2 to default VESC, więc OK"** — sprawdziłem `valInt` w VESC Tool. Ale firmware
  ma `MCCONF_FOC_SAT_COMP_MODE = SAT_COMP_DISABLED`. Wycofałem wycofanie po przeczytaniu kodu.
- **Test ręką** — patrz 3.3. Bezwartościowy, a przez tygodnie traktowaliśmy go jak dowód.

---

# CZĘŚĆ 3 — CO JEST ŻYWE

## 3.1 🎯 Główny podejrzany: strojenie Refloat × wersja firmware

Zweryfikowane w lokalnym źródle `refloat-main/src/` (katalog jest w repo, gitignored):

```c
// pid.c:69 — przy bujaniu DOMINUJE ten człon
pid->rate_p = -imu->pitch_rate * config->kp2 * TORQUE_CONSTANT_COMPAT;

// motor_data.c:103-108 — konwersja moment → prąd
float flux_linkage = VESC_IF->get_cfg_float(CFG_PARAM_foc_motor_flux_linkage);
if (flux_linkage > 0.001f && motor_poles > 0)
    m->speed_constant = 1 / (1.5f * 0.5 * motor_poles * flux_linkage);  // FW 6.06
else
    m->speed_constant = 1 / TORQUE_CONSTANT_COMPAT;                     // FW < 6.06

// lib/utils.h:38
#define TORQUE_CONSTANT_COMPAT (1.5f * 15 * 0.027f)   // = 0,6075
```

| | ADV2 (6.05) | GAD (6.06) |
|---|---|---|
| `kp2` | 0,7 | **0,9** |
| konwersja moment→prąd | flux niedostępny → stała **się skraca** → ×1,0000 | flux 0,028 → kt 0,630 → **×0,9643** |
| **efektywne wzmocnienie rate** | 0,7000 | **0,8679 = +24%** |

Rachunek na zmierzonych liczbach (mnożnik 4,2×):
```
GAD jak jest:            baza 62 A → pik 261 A → FAULT
GAD ze strojeniem ADV2:  baza 50 A → pik 210 A → BEZ FAULTU (limit 225)
```

**⚠️ SŁABE OGNIWO, mów o nim otwarcie:** mnożnik 4,2× pochodzi z **jednego** zdarzenia.
Traktowanie go jak stałej to ekstrapolacja. Hipoteza jest falsyfikowalna i o to chodzi.

## 3.2 ❓ Pytanie bez odpowiedzi — SEDNO SPRAWY

**Skąd bierze się transjent 4× ponad prąd filtrowany?**
Żadna różnica konfiguracji tego nie tłumaczy. Strojenie i masa kierowcy decydują tylko o tym,
**CZY** ten transjent przekroczy 225 A. Sam transjent to prawdziwa anomalia.

To jest pytanie do konsultacji, nie „jak wyłączyć fault".

## 3.3 ⚖️ Masa kierowcy 106 kg — unieważnia dotychczasową metodykę

`kt = 0,63 Nm/A`, środek ciężkości ~1 m nad osią. Prąd na **samo utrzymanie pochylenia** stojąc:

| pochylenie | 106 kg | 70 kg | ręką (bez kierowcy) |
|---|---|---|---|
| 1° | 29 A | 19 A | **0 A** |
| **2°** | **58 A** | 38 A | **0 A** |
| 3° | 86 A | 57 A | **0 A** |

Przy mnożniku 4,2× próg 225 A pęka powyżej **53 A bazy**.

**TEST RĘKĄ / KOŁEM W POWIETRZU JEST BEZWARTOŚCIOWY.** Sama deska (16 kg) przy 2 m/s² daje
~6 A — 10× poniżej progu. Żeby dobić do 225 A trzeba by rozpędzać ją z 18 m/s².
To unieważnia „15 czystych załączeń przy kole w powietrzu" jako dowód czegokolwiek.
**Testy tylko stojąc na desce, z asekuracją, w kasku.**

Kierowca 70 kg mógłby nigdy nie zobaczyć tej usterki na tym samym sprzęcie.

## 3.4 🟡 Żywe, ale poza reżimem awarii
`foc_observer_type` 3 vs **2** · `foc_sl_erpm` 2300/2000 · `foc_openloop_rpm` 1000/700 ·
`foc_fw_duty_start` 0,7/0,65 — wszystkie działają **powyżej 6,3 km/h**, faulty są przy 2 km/h.

## 3.5 Czy to na pewno ten sam sterownik? — do potwierdzenia
**XML nie zawiera ŻADNEGO pola sprzętowego.** Poszlaki:
offsety prądu różnią się o **0,92 LSB z 4096 (0,022%)**, oba ~14 LSB poniżej środka ADC ·
`foc_dt_us` 0,12 · `imu_conf.type` 1 · `imu_conf.sample_rate_hz` 832 · `shutdown_mode` 7.
`hw_adv500.h` to 28 linii: `#define HW_NAME "ADV500"` + `#include "hw_adv_core.h"`.

**Zadanie dla użytkownika (2 min):** odczytać `HW_NAME` + wersję FW z ADV2 w VESC Tool.
Dla GAD wiadomo: `adv500-lckx`, 6.06. Na tym założeniu stoi całe porównanie R/L/flux.

---

# CZĘŚĆ 4 — PLAN TESTÓW (zatwierdzony przez użytkownika)

Zasady: **jeden parametr naraz** · deska stoi przy zapisie · po każdej zmianie eksport XML
do `data/boards/GAD/` + diff · przy każdym faulcie natychmiast `faults` i zrzut.

> **Reguła trzech:** żeby twierdzić, że wskaźnik spadł poniżej 5%, potrzeba **~60 czystych prób**.
> Przy 20 bez kopnięcia wiadomo tylko, że jest poniżej 15%. **Nie ogłaszać sukcesu po 20.**
> Mocniejszy dowód niż licznik: **porównanie pik/filtr i duty@RPM z tabelą 1.2** — działa na
> jednym zdarzeniu.

### Test 1 (NASTĘPNY W KOLEJCE) — `kp2` 0,9 → 0,7
`App Cfg → Refloat → Tune → Rate P`. Jedna zmiana.
- **Przewidywanie:** baza ~62 → ~50 A, pik ~261 → ~210 A → poniżej 225 = brak faultu
- **Falsyfikacja:** fault z pikiem >225 A przy wyraźnie niższej bazie ⇒ hipoteza pada
- **Koszt:** kp2 to człon tłumiący. Deska mniej „przyklejona", miększa na szybkie korekty.
  Przy oscylacji na prędkości — **cofnąć natychmiast**

### Test 2 — `foc_motor_r` 0,0798 → 0,050 **ORAZ** `foc_current_ki` 79,8 → 50 (sugestia Nica)
Oba pola naraz — VESC Tool nie przelicza ki automatycznie (`kp = L×bw`, `ki = R×bw`, bw=1000).
- Łagodzi integrator o 37%, działa przy każdej prędkości
- ⚠️ **Jedyna zmiana z ryzykiem przy prędkości:** R używa obserwator powyżej ~7 km/h.
  Zaniżenie o 30 mΩ przy 60 A ⇒ **~17° błędu pozycji na progu bezczujnikowego**,
  ~7° @17 km/h, ~3° @35 km/h
- Przy częściowej poprawie: druga tura z `L` 128,9 → 82 µH (spójna korekta hipotezy 3/2)

### Test 3 — `foc_observer_type` 2 → 3
Dla domknięcia listy. Przy 2 km/h obserwator nie wyznacza pozycji, więc raczej nie ruszy objawu.

### CZEGO NIE ROBIĆ
❌ Podnosić `l_abs_current_max` (patrz 2.4) · ❌ `foc_hall_interp_erpm` (2.5) ·
❌ Detekcji silnika (zmienia R, L, flux i halle naraz) ·
❌ Mieszać `bms.enabled` / `hardware.leds.pin` — zaburzą eksperyment

---

# CZĘŚĆ 5 — DRUGI WĄTEK: telemetria (Cardputer + HUD)

Sprzęt: **M5 Cardputer** (nadajnik, BLE do deski, GPS, zapis na SD) → **ESP-NOW** →
**M5 AtomS3R** (HUD na kask, ekranik + panel LED 16×8).

**Zrobione i wgrane:** naprawa SOC baterii w HUD · dual-source (HUD łączy się bezpośrednio
po BLE, gdy nie ma Cardputera) · wybór deski (klik = następna, długie = potwierdź, potrójny
klik przy v=0 = wybór od nowa) · kalibracja prędkości per deska (ADV2 240 mm, GAD/default
234 mm, wszystkie 15 par biegunów; wartości z GPS, ~6% poniżej nominału opony).

**⚠️ NIESCOMMITOWANE I NIEWGRANE: `helmet/atoms3r_hud/atoms3r_hud.ino`, +235 linii.**
To BLE relay — HUD udaje VESC-a, żeby telefon (Float Control) mógł dojść do deski przez niego.
Naprawione po drodze: crash przy connect (zapisy BLE przeniesione z callbacków do kolejek
i `relayPump()` w loop) · `assert ble_svc_gap_init` (GATT budowany raz w `setup()`) ·
niewidoczność w skanie (nazwa w reklamie, UUID w scan response — 31 bajtów) · „łączy się,
ale nie ładuje" (zaszyte 180 B zamiast negocjowanego MTU, bufor 2 KB gubił bajty, HUD wstrzykiwał
własne pollingi w handshake apki).

**Stan: kod na dysku, nie w gicie, nie przetestowany na sprzęcie.** Ryzyko: `git checkout`
albo `git stash` na tym pliku i przepada. Zapytaj użytkownika, czy zabezpieczyć commitem
z adnotacją „untested".

---

# CZĘŚĆ 6 — GDZIE CO LEŻY

```
data/boards/ADV2_orig/ADV2_{Motor,Refloat,App}Cfg_2026-08-15.xml   ← deska, która DZIAŁA
data/boards/GAD/GAD_*Cfg_2026-08-09{,_post-floathub}.xml           ← przed/po Float Hub
data/boards/GAD/GAD_*Cfg_2026-08-15.xml                            ← przed zmianami 17.08
data/boards/GAD/GAD_MotorCfg_2026-08-17.xml                        ← STAN OBECNY DESKI
data/boards/GAD/CONSULTATION_2026-08-17.md                         ← materiał EN dla Nica/forum
data/boards/GAD/bmslog_GAD_19_2026-08-09.csv
sessions/                          ← ~20 sesji CSV 25 Hz (gitignored, nie commitować)
refloat-main/                      ← źródła Refloat (gitignored, lokalna referencja)
web/params.js                      ← katalog parametrów; enum observera POPRAWIONY 17.08
~/.claude/plans/rustling-pondering-treehouse.md   ← pełny plan testów
```

**Nie commitować:** `SD /` (są tam klucze RFID `BruceRFID/keys.conf`) i `sessions/`.
Oba są untracked i nie ma ich w `.gitignore` — będą wyskakiwać przy `git status`.

**Źródła zewnętrzne, z których korzystałem:**
- `surfdado/bldc` gałąź `v66_pinlock` (firmware GAD) i `ADV_Vanilla_v605` (fabryczny)
- `vedderb/bldc` (upstream), `vedderb/vesc_tool` (`res/config/6.06/parameters_mcconf.xml`)
- Kluczowe pliki: `motor/foc_math.c` (`foc_observer_update`, `foc_correct_hall`, `foc_run_fw`),
  `motor/mcconf_default.h`, `motor/mcpwm_foc.c:491` (kalibracja offsetów przy boocie)
- Uwaga: GitHub API rate-limituje przy intensywnym pobieraniu — rób przerwy

---

# CZĘŚĆ 7 — OTWARTE, DROBNE

- [ ] **Odczytać `HW_NAME` z ADV2** — potwierdza fundament porównania (2 min)
- [ ] **Test 1: `kp2` 0,9 → 0,7** — następny krok merytoryczny
- [ ] Zdecydować co z niescommitowanym `atoms3r_hud.ino`
- [ ] `hardware.leds.pin` 0 → 1 (LED-y nie świecą; Float Hub to zmienił) — **po zakończeniu testów**
- [ ] `bms.enabled` = 0 → włączyć z progami per cela (3,00 V / 4,25 V / 55 °C) — **po testach**
- [ ] Wysłać materiał konsultacyjny do Nica i na forum
- [ ] Zgłosić Dado: blokada field weakening z release notes nie istnieje w żadnej wydanej gałęzi
- [ ] W backupach VBK2 **appconf ma 0 bajtów** — nigdy się nie zapisuje (bug do naprawy)
- [ ] `tiltback_duty` na desce 0,75, a `device_profile.json` na SD ma 80 — Duty-Geiger
      Cardputera jest ustawiony 5 pkt za wysoko
- [ ] Do weryfikacji: notatka, jakoby Refloat 1.2 miał progi tiltback **per cela**. Dane sugerują,
      że to pole jest napięciem pakietu (ADV2 ma 88/58 przy identycznym zestawie pól).
      Nie powielać bez sprawdzenia w kodzie.

---

# CZĘŚĆ 8 — JEDNOZDANIOWE PODSUMOWANIE

Deska GAD (ADV500 + Sidewinder) daje transjent prądu **4× ponad prąd filtrowany** przy
**~2 km/h w trybie hallowym**, co przy kierowcy 106 kg (baza ~60 A) przebija limit 225 A
i ucina napęd; halle, bateria, temperatura, limity i parametry elektryczne silnika zostały
**policzone i wykluczone**, aktualny podejrzany to **+24% wzmocnienia członu prędkościowego
Refloat** (kp2 0,9 vs 0,7 × różnica konwersji moment→prąd między FW 6.05 a 6.06),
a **przyczyna samego transjentu pozostaje nieznana** i to jest właściwe pytanie.
