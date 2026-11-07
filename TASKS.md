# httpdate

`httpdate` synchroniseert de tijd met behulp van de `Date`-header in HTTP `HEAD`-responses.

Door meerdere HTTP(S)-requests rond een secondewisseling uit te voeren, kan de synchronisatie millisecondenprecisie bereiken.

# Tasks

## 1. Historie mee laten wegen

v Bewaar per URL de laatste 8 samples, waaronder offset en jitter, om de kwaliteit van de URL te bepalen.
v Gebruik deze historie voor betere tijdsynchronisatie.

## 2. Minimale NTP-server

v Implementeer een minimale NTP-server die met `httpdate` integreert.
v Zet de servercode in een apart bestand: `ntpd.c`.
v Start de server met de optie `-S <1..9>`.
  v De server luistert op UDP-poort 123.
  v De optionele waarde `<1..9>` bepaalt het stratum.
v Integreer de server met `httpdate.c`.
v Pas de `Makefile` aan.
v Wijzigingen:
  v stratum is fixed 5
  v `-S` optie zonder argumenten, start alleen als ook `-D` of `-F` zijn gezet
v Update README en man page (httpdate.8)
v Maak het productiekwaliteit
- Update 'systemd integration' in README.md, allow bind poort 123
