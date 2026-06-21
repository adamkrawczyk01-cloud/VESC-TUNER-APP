# Ride assessment — GAD Ride #17

**Data:** 2026-06-19 · **Źródło:** Float Control (bez mcconf → progi COMMUNITY/CHEM) · **Pakiet:** 20S · **Pogoda:** ~22°C, sucho

## Werdykt: 🟢 zdrowy przejazd — B+
Spokojna, wydajna jazda głęboko w bezpiecznej kopercie termicznej i prądowej. Dwa punkty uwagi: krótkie skoki duty (szczyt 95%) oraz **buzze HV przy hamowaniu pod koniec = oznaka rozjazdu balansu cel**.

## Metryki (68,9 km · 190 min jazdy / 466 min z przerwami)

| Obszar | Wynik | Ocena |
|---|---|---|
| Wydajność | **16,2 Wh/km** netto (1355 Wh zuż., 241 Wh regen = 18% odzysku) | 🟢 good (COMMUNITY good 15 / avg 22) |
| Termika FET | max **44°** (warn 70°), przyrost 2,5°C/min | 🟢 ogromny zapas |
| Termika silnik | max **65,5°** (warn 75°), przyrost 5°C/min | 🟢 ok |
| Termika bateria | max **45,5°** (warn 50°) | 🟢 ok |
| Duty / prędkość | top **42,8 km/h**, duty max **95%**, tylko 0,01% czasu >80% | 🟡 przelotne piki |
| Napięcie | 60,2–85,7 V (**3,01–4,29 V/cela**), sag do ~14,5 V (≈0,7 V/cela) | 🟡 spory sag |
| Prąd | silnik do **128A** / regen **−117A**, bateria **40A** | 🟡 lekko ponad COMMUNITY (120/30A) |
| Dynamika (pochodna) | lean **72°**, pitch-rate **96°/s**, roll-rate **81°/s** | energiczna jazda |

## Główne ustalenie — buzz HV pod koniec
9 zrekonstruowanych okien HV + 4 alerty „Cell voltage high" (2 w końcówce przy hamowaniu, prąd silnika ujemny). Mechanizm: mocna cela dobija do limitu HV przy regenie, gdy pakiet się rozładowuje → **rozjazd balansu cel**.

## Zalecenia
1. **Balans-charge** (priorytet) — usuwa buzz HV przy hamowaniu i oznaki rozjazdu cel.
2. Nie dociskać prędkości na niskiej baterii (piki 95% duty).
3. Wczytać **mcconf z sesji Cardputera** tej deski → ocena prądu/temp wskoczy na twardy tier BOARD.

## Ustawienia VESC
**Nie zmieniamy** — patrz osobna notatka. Ride mieści się w kopercie; jedyne działanie to balans-charge (obsługa baterii, nie parametr).

---
*Wygenerowane z analizy CSV; progi COMMUNITY/CHEM (brak mcconf). Tryb READ-ONLY — żadne zmiany nie idą do VESC.*
