# Morse TX — Flipper Zero

App (FAP) che trasmette messaggi di testo in **codice Morse** manipolando la portante
del **modulo radio esterno CC1101** collegato al connettore GPIO.

Il messaggio viene compilato in una sequenza di elementi `livello/durata` (portante ON per
punti e linee, OFF per le pause) e passata al trasmettitore asincrono sub-GHz: in pratica è
un manipolatore CW/OOK, non una modulazione dati.

## File

| File | Contenuto |
|---|---|
| `application.fam` | manifest dell'app |
| `morse.h` / `morse.c` | tabella Morse e generatore della sequenza di manipolazione |
| `morse_tx.c` | interfaccia (menu, impostazioni, tastiera, schermata TX) e gestione radio |

## Compilazione

Serve **ufbt** (Micro Flipper Build Tool):

```bash
python -m pip install --upgrade ufbt
```

Poi, dentro la cartella del progetto:

```bash
ufbt
```

Il `.fap` viene generato in `dist/morse_tx.fap`. Per compilare, caricare e lanciare
direttamente sul Flipper collegato via USB:

```bash
ufbt launch
```

In alternativa copia `dist/morse_tx.fap` sulla microSD in `/ext/apps/Sub-GHz/`.

Il `.fap` è legato alla versione di API del firmware, quindi va compilato con l'SDK del
firmware installato sul Flipper. Per **Unleashed**:

```bash
ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release
```

Per tornare al firmware **ufficiale**:

```bash
ufbt update --channel=release
```

Compilato senza warning sia con l'SDK ufficiale 1.4.3 (API 87.1) sia con Unleashed
unlshd-092 (API 88.4). Su firmware < 0.99 l'API `subghz_devices` non esiste e il progetto
non compila.

## Collegamento del modulo CC1101 esterno

| CC1101 | Flipper GPIO |
|---|---|
| GDO0 | pin 2 (A7) |
| MISO | pin 3 (A6) |
| CS   | pin 4 (A4) |
| SCK  | pin 5 (B3) |
| MOSI | pin 6 (B2) |
| GND  | pin 8 / 11 / 18 |
| VCC  | pin 9 (3V3) oppure pin 1 (5V) |

Se il modulo va alimentato a 5V, lascia **`5V on GPIO` = ON** nelle impostazioni: l'app
abilita l'OTG all'inizio della trasmissione e lo disabilita alla fine (se non era già
attivo). È lo stesso schema dei moduli esterni ufficiali.

**Attenzione:** i 5V sul pin 1 arrivano dal boost OTG del caricabatterie, che **non può
partire mentre il cavo USB è collegato** (in quella condizione VBUS è un ingresso).
`furi_hal_power_enable_otg()` restituisce `false` e il pin 1 resta morto: un modulo
alimentato a 5V risulta quindi assente finché il Flipper è attaccato al PC. Per usarlo
scollega l'USB, oppure alimenta il modulo dal 3V3 (pin 9). L'app rileva questa condizione
e lo dice esplicitamente invece di limitarsi a "modulo non rilevato".

All'avvio l'app cerca il modulo esterno (`subghz_devices_is_connect()`, con 5V acceso
momentaneamente per la sonda) e imposta `Module` di conseguenza: **External** se c'è,
altrimenti **Internal**, così funziona anche senza modulo. La scelta resta modificabile
a mano in Settings.

Nota sul driver: `subghz_devices_begin()` va **sempre** chiuso con `subghz_devices_end()`,
anche quando fallisce, altrimenti il tentativo successivo va in assert.

### Rilevamento del modulo esterno

`subghz_devices_is_connect()` **non è affidabile** su tutte le schede: su quella provata
qui (CC1101 di kasiin, pinout standard) restituisce sempre `false` anche quando il chip
risponde correttamente e `subghz_devices_begin()` dello stesso driver ritorna `true`. Per
questo l'app identifica il chip da sé, leggendo i registri di stato via SPI sul bus
`furi_hal_spi_bus_handle_external`:

- `PARTNUM` (0x30) e `VERSION` (0x31) si leggono con il bit di burst: header `addr | 0xC0`.
- Un CC1101 vero risponde `partnum 0x00`, `version 0x14` (alcuni cloni `0x04`/`0x17`).
- Se la versione è quella giusta si procede, e la trasmissione resta in carico al driver.

Prima di ogni transazione il CS viene "risvegliato" (`morse_tx_cs_wakeup()`): rilasciato a
input per un paio di ms e poi ripilotato alto. Senza questo, subito dopo un init fallito
del driver la linea resta asserita, il chip non vede il fronte di discesa e la lettura
successiva torna tutta a zero — è esattamente l'errore che faceva sembrare il modulo assente.

## Uso

Menu principale:

- **Transmit** — avvia la trasmissione.
- **Message** — tastiera per scrivere il messaggio (max 64 caratteri, convertito in maiuscolo).
- **Settings** — frequenza, velocità, ripetizioni, modulo, 5V, sidetone.
- **Radio check** — diagnostica: dice se il driver `cc1101_ext` è caricato e sonda il
  modulo esterno due volte, senza e con i 5V, così si distingue "driver assente" da
  "modulo non alimentato" da "modulo che non risponde sull'SPI". Se lo trova, seleziona
  automaticamente `Module = External`.
- **About** — pinout, timing e nota legale.

Impostazioni:

| Voce | Valori | Note |
|---|---|---|
| Frequency | 300 – 915 MHz | lista di frequenze ISM comuni; validate dal driver |
| Speed | 5 – 40 WPM | standard PARIS: unità = 1200 ms / WPM |
| Repeat | 1, 2, 3, 5, 10, loop | `loop` ripete finché non premi Back |
| Module | External / Internal | preselezionato all'avvio in base a cosa è collegato |
| 5V on GPIO | ON / OFF | alimentazione OTG per il modulo esterno |
| Sidetone | ON / OFF | tono di monitoraggio a 700 Hz dallo speaker |

Durante la TX lo schermo mostra frequenza, velocità, indicatore `AIR`, il messaggio con il
carattere in corso evidenziato, i punti/linee del carattere, la barra di avanzamento e il
tempo. Il LED rosso lampeggia in sincrono con la manipolazione. **Back** interrompe subito
la trasmissione e spegne la radio.

Caratteri supportati: A–Z, 0–9, spazio e `. , ? ' ! / ( ) & : ; = + - _ " $ @`.
Tutto il resto viene ignorato.

## Stato

Compilata, installata e provata su hardware (Flipper Zero con Unleashed unlshd-092, modulo
CC1101 esterno di kasiin). Trasmissione di `CQ CQ DE FLIPPER` a 40 WPM su 433.92 MHz:

```
module: External      started: yes       completed: yes
elapsed ms: 4873      marcstate: 0x13 (TX)
```

`MARCSTATE = 0x13` è lo stato TX del CC1101 letto *durante* la trasmissione, quindi il chip
stava effettivamente irradiando. I 4873 ms misurati corrispondono ai 162 unità × 30 ms
(= 4860 ms) previsti dallo standard PARIS per quel messaggio a 40 WPM: 0,3% di scarto.

## Timing

Con unità `T = 1200 ms / WPM`:

- punto = 1T portante ON, linea = 3T ON
- pausa tra i simboli di una lettera = 1T
- pausa tra lettere = 3T
- pausa tra parole = 7T

Le pause non vengono mai emesse come due elementi consecutivi allo stesso livello: l'HAL
sub-GHz si aspetta livelli strettamente alternati, quindi la pausa esistente viene allungata.

## Note tecniche

- Il callback `morse_tx_yield()` gira in contesto DMA/interrupt: si limita a estrarre un
  elemento già calcolato dall'array, nessuna allocazione.
- Il buffer DMA viene riempito in anticipo rispetto all'emissione reale, quindi
  avanzamento, sidetone e LED sono agganciati all'orologio di sistema (`furi_get_tick()`)
  e non al cursore dell'encoder.
- Preset radio: `FuriHalSubGhzPresetOok650Async`.

## Licenza

**GNU General Public License v3.0 o successiva** ([LICENSE](LICENSE)).

Il programma deve restare open source: chi lo ridistribuisce, modificato o no, deve
distribuirlo sotto la stessa licenza e rendere disponibile il codice sorgente. È la stessa
licenza del firmware del Flipper Zero.

## Nota legale

L'app emette una portante reale. Frequenza, potenza, duty cycle e necessità di licenza
dipendono dal Paese: usala solo su bande e con modalità consentite (per esempio in ambito
radioamatoriale con nominativo valido, o entro i limiti ISM/SRD locali).
