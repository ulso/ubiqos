# myrtos

Ett litet realtidsoperativsystem för RISC-V, i OS-9:s anda: **positionsoberoende
kod** och ett **modulsystem** där kod delas mellan processer i stället för att
laddas om en gång per process.

Målet är Hazard3-kärnan i RP2350 på ett **Adafruit Fruit Jam**. RP2350 har två
ARM Cortex-M33 *och* två RISC-V-kärnor; boot-ROM:et läser
`PICOBIN_IMAGE_TYPE_EXE_CPU` ur flashavbilden och växlar till RISC-V först om
avbilden säger det. Därför byggs allt med `PICO_PLATFORM=rp2350-riscv`, och
därför måste systemet ligga i flash — en ren RAM-avbild ger boot-ROM:et inget
att växla på.

## Vad som finns

- Förebyggande växling på maskintimern, 1 ms kvantum, upp till 8 processer
- TLSF-heap på 320 kB, med splittring och sammanslagning av block
- Moduler laddade från FAT32 på SD-kortet, eller funna residenta i flash
- Moduldirektorium med länkräkning — en modul som redan är inne delas
- I/O-hanterare med enhetsbeskrivare; konsol på både UART och USB CDC
- Vägnummer per process som ärvs vid `exec`: 0 stdin, 1 stdout, 2 stderr
- Ett skal, `sh`, som startar moduler med argument och `argc`/`argv`

## Bygga

Kräver Pico SDK 2.2.0 och RISC-V-verktygskedjan som följer med den.

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0
export PATH=$HOME/.pico-sdk/toolchain/RISCV_ZCB_RPI_2_2_0_3/bin:$PATH
cmake -S . -B build -G Ninja && ninja -C build
```

Det ger `build/os_kernel.uf2` och en `.mod`-fil per modul.

## Flasha

Håll **BOOTSEL** nedtryckt och tryck **RESET**, sedan:

```bash
picotool load -x build/os_kernel.uf2
```

**J-Link kommer inte in.** En Segger J-Link Ultra+ kan halta kärnan och läsa och
skriva RAM, men vägrar programmera flashet (`Failed to read back RAMCode`).
Orsaken är inte utredd. BOOTSEL och `picotool` fungerar och är vägen som används.

## Modulsystemet

En modul är en fil med ett huvud, ingen ELF och ingen laddare som relokerar. Den
körs där den ligger — från flash utan att kopieras, eller från en kopia i heapen
när den kom från kortet. Huvudet ligger i [`common/myrtos_abi.h`](common/myrtos_abi.h)
och säger bland annat hur mycket RAM processen behöver: dataområdet växer nedifrån
och stacken från toppen, i samma block.

Att flera processer kan dela samma kod är hela poängen, och det bygger på att
koden inte innehåller några absoluta adresser.

### Positionsoberoende utan GOT

Modulerna byggs med `-fno-pic -mcmodel=medany`, **inte** `-fPIC`. Det är
kontraintuitivt: `-fPIC` skapar en GOT, och GOT-posterna fylls med adresser som
gäller där modulen länkades. En modul som laddas någon annanstans läser då fel.
`medany` ger i stället `auipc`-baserad adressering, PC-relativ på riktigt.

Kravet kontrolleras vid bygget. [`check_module.py`](check_module.py) läser
relokeringarna ur objektfilerna med `readelf -W` och sågar modulen om någon
allokerad sektion innehåller en absolut referens som `R_RISCV_32`. Utan den
kontrollen visar sig felet först som en krasch efter laddning, på en adress som
inte säger något.

### Bygga en egen modul

Lägg källkoden under `modules/` och registrera den i `CMakeLists.txt`:

```cmake
myrtos_add_module(mitt modules/mitt/mitt.c)
```

Modulen börjar i `module_main`, som får kommandoraden:

```c
#include "../../common/myrtos_abi.h"

void module_main(int argc, char **argv) {
    myrtos_line_t line;
    myrtos_line_reset(&line);
    for (int i = 1; i < argc; i++) myrtos_line_str(&line, argv[i]);
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);
}
```

`argv[0]` är modulens eget namn ur huvudet. Kärnan kopierar kommandoraden till
botten av processens minnesområde, delar den där med citattecken hanterade, och
bygger argv-vektorn efter strängen — ingen extra allokering, inget att frigöra.

## Få in moduler i systemet

**Från SD-kort.** Kopiera `.mod`-filerna till kortets rot. Filsystemsstödet är
FAT32 och läsbart bara; kärnan söker filer med ändelsen `MOD` och registrerar
dem vid uppstart. Skriv `sh.mod` som `SH.MOD` — 8.3-namn.

**Residenta i flash.** Slå ihop modulerna till en avbild och lägg den i
modulregionen, `0x10100000`–`0x11000000`. Där finns ingen katalog: kärnan söker
efter synkordet, precis som OS-9 gjorde med ROM — modulen *är* sin egen
katalogpost.

```bash
python3 make_flash_image.py build/modules.bin \
        build/sh.mod build/echo.mod build/lsmod.mod \
        build/free.mod build/termdesc.mod build/usbdesc.mod
picotool load -t bin -o 0x10100000 build/modules.bin
```

Flashen söks igenom före kortet. En modul med samma namn på kortet hamnar
bredvid, och den som registrerades först vinner uppslagningen.

## Enheter

En enhet läggs till genom att släppa en beskrivare i systemet, inte genom att
bygga om kärnan. Beskrivaren är en datamodul — ingen kod, ingen startpunkt — som
säger vad enheten heter, vilken drivrutin som hanterar den, och bär en svans som
bara den drivrutinen förstår. Vill man byta UART eller baudrate ändrar man
beskrivaren.

| Beskrivare | Enhet  | Drivrutin     |
|------------|--------|---------------|
| `termdesc` | `term` | `UART    MOD` |
| `usbdesc`  | `usb`  | `USBCDC  MOD` |

Saknas beskrivare helt registrerar kärnan en inbyggd UART-konsol, så systemet
aldrig blir stumt.

## Uppstart

1. Schemaläggare, I/O-hanterare och moduldirektorium initieras
2. Flashen söks igenom efter residenta moduler
3. SD-kortet monteras och `.MOD`-filer registreras
4. Datamoduler tolkas som enhetsbeskrivare och enheterna skapas
5. Finns `sh` startas bara det, med 0/1/2 öppnade mot `usb`, annars `term`
6. Saknas skal startas allt som finns, så systemet ändå visar livstecken
7. Timern startas och förebyggande växling börjar

## Konsolen

Kortet dyker upp som en USB-serieport. Anslut med

```bash
screen /dev/cu.usbmodem0000011 115200
```

Enhetsnamnet varierar mellan kort; `ls /dev/cu.usbmodem*` visar vilket.

UART-konsolen finns parallellt på **GP44** (märkt A4 på listen) i 115200 baud,
men bara som utmatning — beskrivaren sätter `rx_pin` till `0xffffffff`, så den
tar inte emot tecken. Kärnans uppstartsutskrifter går att följa där, medan
skalet bara går att styra över USB.

## Skalet

```
myrtos> help
Type a module name to run it. Built in:
  help   this text
  lsmod  list modules
  free   memory and processes
  echo   print its arguments
```

`help` är det enda skalet gör själv. Allt annat är moduler som slås upp i
directoriet och startas — `lsmod`, `free`, `echo`, `counter`, `cxxdemo`,
`usbecho`. Skalet öppnar ingenting; det ärvde 0, 1 och 2 av kärnan, och allt det
startar ärver dem i sin tur, så ett verktyg varken öppnar eller känner till
någon enhet.

## Systemanrop

`a7` bär numret, `a0`–`a2` argumenten, `a0` kommer tillbaka med resultatet.
Inlinefunktioner för alla finns i ABI-huvudet.

| Nr | Namn | Argument |
|----|------|----------|
| 0 | `SYS_NULL` | — |
| 1 | `SYS_IO_PUTC` | tecken |
| 2 | `SYS_EXIT` | — |
| 3 | `SYS_OPEN` | enhetsnamn → vägnummer |
| 4 | `SYS_WRITE` | väg, buffert, längd |
| 5 | `SYS_CLOSE` | väg |
| 6 | `SYS_MODDIR` | index, buffert → länkar |
| 7 | `SYS_MEMINFO` | vad → värde |
| 8 | `SYS_READ` | väg, buffert, längd → lästa |
| 9 | `SYS_EXEC` | modulnamn, argument → pid |
| 10 | `SYS_ARGS` | buffert, längd → kopierade tecken |

Trap-vektorn hänger på SDK:ns svaga vektorsymboler i stället för att äga `mtvec`
själv. Det var inte det första försöket: att ta `mtvec` fungerade tills TinyUSB
skulle initieras och hårdassade i `irq_add_shared_handler`. Att haka i i stället
för att slåss tog bort mer kod än det lade till.

## Vad som inte finns

- **Ingen skrivning till filsystemet.** FAT32-stödet är läsbart bara, så
  `rm`, `cp` och `mkdir` går inte att skriva än.
- **Ingen mekanism att sova.** En process som väntar på indata pollar, och
  tomgång bränner kvanta. Blockerande läsning är nästa riktiga steg.
- **Inget skydd.** Ingen MPU, inget användarläge — en modul kan skriva var som
  helst. Positionsoberoendet är en förutsättning för delning, inte en spärr.
