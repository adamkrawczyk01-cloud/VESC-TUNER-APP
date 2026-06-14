# VESC AI Tuner for M5Stack Cardputer

Narzędzie do precyzyjnego tuningu kontrolerów VESC na urządzeniu M5Stack Cardputer.
Loguje telemetrię jazdy, snapshot konfiguracji VESC i generuje bezpieczne sugestie zmian
z uzasadnieniem opartym na rzeczywistych danych.

## Architektura

```
┌─────────────────────────────────────────────────────────────┐
│  M5Stack Cardputer                                          │
│                                                             │
│  1. START SESJI:                                            │
│     COMM_GET_MCCONF (cmd 14) → session_XXX_mcconf.json      │
│     COMM_GET_APPCONF (cmd 16) → session_XXX_appconf.json    │
│                                                             │
│  2. JAZDA:                                                  │
│     telemetria 12Hz → session_XXX.csv                       │
│     dashboard: prędkość, duty, V/celę, A, W,               │
│     temp FET/silnika, pitch angle, bateria, trip            │
│                                                             │
│  3. PO SESJI:                                               │
│     USB mass storage → Mac kopiuje pliki z SD               │
│     wczytuje suggestions.json → review [Y/N]               │
│     zaakceptowane zmiany → VESC przez BLE                   │
│     weryfikacja: GET_MCCONF po każdym SET_MCCONF            │
└───────────────────┬─────────────────────────────────────────┘
                    │ SD card (most danych)
        ┌───────────▼───────────┐      ┌────────────────────┐
        │  data/sessions/       │      │  data/pev-reference/│
        │  session_XXX.csv      │      │  tune_benchmarks    │
        │  session_XXX_mcconf   │      │  float_package_     │
        │  session_XXX_appconf  │      │    params           │
        └───────────┬───────────┘      │  safety_limits_20s  │
                    │                  └────────────┬────────┘
        ┌───────────▼──────────────────────────────▼────────┐
        │  Mac + Claude Code                                 │
        │                                                    │
        │  analiza CSV + porównanie z aktualnym mcconf       │
        │  → config/suggestions.json                         │
        │    (tylko zmiany względem bieżącego configu,       │
        │     max ±15%, whitelist 5 parametrów)              │
        └────────────────────────────────────────────────────┘
```

## Struktura projektu

```
vesc-tuner/
├── cardputer/              # Kod Arduino (.ino) dla M5Stack Cardputer
├── config/
│   └── suggestions.json    # Sugestie zmian VESC (Mac → Cardputer)
├── data/
│   ├── sessions/           # Dane sesji (Cardputer → Mac)
│   │   ├── session_001.csv
│   │   ├── session_001_mcconf.json
│   │   └── session_001_appconf.json
│   └── pev-reference/      # Dane referencyjne z pev.dev
│       ├── tune_benchmarks.json
│       ├── float_package_params.json
│       └── safety_limits_20s.json
├── analysis/               # Wyniki analizy sesji
└── README.md
```

## Hardware

- **M5Stack Cardputer** — kompaktowy komputer z klawiaturą, ekranem i czytnikiem SD
- **VESC** — kontroler silnika elektrycznego (Vedder Electronic Speed Controller)
- **Bateria 20S** — 72–84V, bezpieczne zakresy pilnowane przez safety_limits_20s.json
- **Karta SD** — asynchroniczny most danych między Cardputerem a Macem

## Format danych sesji

### CSV telemetrii (12Hz)
```
ts_ms,speed_kmh,rpm,voltage_V,curr_in_A,curr_mot_A,duty_pct,temp_fet_C,temp_mot_C,amp_hours,tacho
```

### session_XXX_mcconf.json
Pełna konfiguracja silnika odczytana przez `COMM_GET_MCCONF` (cmd 14) na starcie sesji.

### session_XXX_appconf.json
Konfiguracja aplikacji (Float Package) odczytana przez `COMM_GET_APPCONF` (cmd 16) na starcie sesji.

## Format suggestions.json

```json
{
  "session": "session_001",
  "generated_at": "2026-03-24T10:00:00Z",
  "suggestions": [
    {
      "param": "l_current_max",
      "current": 80.0,
      "suggested": 68.0,
      "delta_pct": -15.0,
      "reason": "wykryto 5 spike'ow powyzej 80% limitu przez >200ms",
      "safe": true
    }
  ]
}
```

### Whitelist parametrów (jedyne dozwolone zmiany)
| Parametr | Opis |
|---|---|
| `l_current_max` | Maks. prąd silnika |
| `l_in_current_max` | Maks. prąd wejściowy (bateria) |
| `l_max_erpm` | Maks. prędkość obrotowa |
| `l_temp_fet_start` | Temp. FET — start dławienia |
| `l_temp_fet_end` | Temp. FET — pełne wyłączenie |

**Limit zmiany: ±15% wartości bieżącej naraz.**

## Bezpieczeństwo

- Żadna zmiana nie trafia do VESC bez jawnej akceptacji [Y] na Cardputerze
- Po każdym `SET_MCCONF` — auto-weryfikacja przez `GET_MCCONF`
- Sugestie generowane względem *aktualnego* configu (nie domyślnego) — precyzja bez zgadywania
- Wartości sandboxowane przez `safety_limits_20s.json` przed wygenerowaniem sugestii

## Szybki start

1. Wgraj kod z `cardputer/` na M5Stack Cardputer
2. Podłącz Cardputer do VESC przez BLE
3. Jedź — dane logują się automatycznie
4. Podłącz Cardputer jako USB mass storage do Maca
5. Skopiuj sesję z SD do `data/sessions/`
6. Uruchom analizę → sprawdź `config/suggestions.json`
7. Wróć do Cardputera → przejdź przez review [Y/N]
