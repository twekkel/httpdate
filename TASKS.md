branch prom, deze bevat prometheus support.

plan:
* alleen tekst .prom file support, geen http
* één file voor httpdate en één voor ntpd metrieken
* directory is configurabel als cli optie (-M <dir>)
* alleen schrijven als -M expliciet is gegeven
* httpdate.prom: basisset -- offset (s), resolution (s), rtt (s), aantal geldige servers
* ntpd.prom: request-tellers -- ok / bad / dropped
* atomair schrijven (tmp + rename), zoals node_exporter textfile collector verwacht
* optioneel te bouwen: WITH_METRICS (default aan, uit te zetten met make WITH_METRICS=0),
  vergelijkbaar met de NTP server (WITH_NTP)
