/* ROM access: the game's data is read from the user's own iNES file. */
#include <stdio.h>
#include <string.h>

#include "bd.h"

BdRam ram;
uint8_t bd_bus_scratch;

static uint8_t prg_rom[0x8000];
static uint8_t chr_rom[0x8000];

bool bd_rom_load(const char *path, char *err, size_t errlen)
{
    uint8_t header[16];
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return false;
    }
    bool ok = fread(header, 1, 16, f) == 16 && memcmp(header, "NES\x1a", 4) == 0 &&
              header[4] == 2 && header[5] == 4 &&
              fread(prg_rom, 1, sizeof prg_rom, f) == sizeof prg_rom &&
              fread(chr_rom, 1, sizeof chr_rom, f) == sizeof chr_rom;
    fclose(f);
    if (!ok)
        snprintf(err, errlen, "%s is not Boulder Dash (USA): expected iNES, 32 KB PRG + 32 KB CHR", path);
    return ok;
}

uint8_t prg(uint16_t addr) { return prg_rom[addr & 0x7FFF]; }

uint8_t chr(uint32_t offset) { return chr_rom[offset & 0x7FFF]; }

uint8_t rd(uint16_t addr) { return *bus_ref(addr); }
