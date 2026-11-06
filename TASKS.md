# httpdate

`httpdate` synchroniseert de tijd met behulp van de `Date`-header in HTTP `HEAD`-responses.

Door meerdere HTTP(S)-requests rond een secondewisseling uit te voeren, kan de synchronisatie millisecondenprecisie bereiken.

# Tasks

## Historie mee laten wegen

- Bewaar per URL de laatste 8 samples, waaronder offset en jitter, om de kwaliteit van de URL te bepalen.
- Gebruik deze historie voor betere tijdsynchronisatie.

## Minimale NTP-server

- Implementeer een minimale NTP-server die met `httpdate` integreert.
- Zet de servercode in een apart bestand: `ntpd.c`.
- Start de server met de optie `-S <1..9>`.
  - De server luistert op UDP-poort 123.
  - De optionele waarde `<1..9>` bepaalt het stratum.
- Integreer de server met `httpdate.c`.
- Pas de `Makefile` aan.

