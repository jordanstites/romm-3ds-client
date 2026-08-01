/*
 * ROM format probing
 *
 * Reads the first few KB of a ROM from the server to work out what it is,
 * without downloading it. RomM's per-file endpoint honours HTTP Range, so this
 * costs a couple of small requests.
 *
 * Two things come out of it. The title ID gives an exact link between a RomM
 * entry and an installed 3DS title, replacing a name-matching guess -- which
 * matters because that link decides which save archive gets written. And the
 * encryption state decides what converting a .3ds to an installable .cia would
 * actually involve: repackaging, or repackaging plus decryption.
 */

#ifndef ROMFORMAT_H
#define ROMFORMAT_H

#include <3ds.h>
#include <stdbool.h>

typedef enum {
    ROMFORMAT_UNKNOWN,
    ROMFORMAT_CIA,  // installable as-is
    ROMFORMAT_NCSD, // a .3ds/.cci cartridge image; needs conversion
} RomFormatKind;

typedef enum {
    ROMCRYPTO_UNKNOWN,
    ROMCRYPTO_NONE,     // already decrypted: conversion is repackaging only
    ROMCRYPTO_STANDARD, // original NCCH encryption, needs boot9-derived keys
    ROMCRYPTO_FIXED,    // fixed ("zero") key, trivially handled
    ROMCRYPTO_SEED,     // 9.6+ seed crypto, additionally needs the title's seed
} RomCrypto;

typedef struct {
    RomFormatKind kind;
    RomCrypto crypto;
    u64 titleId;     // 0 when it could not be determined
    u8 cryptoMethod; // raw NCCH flag, for diagnosing an unexpected value
    bool probed;     // false when the server could not be reached
} RomFormatInfo;

// A .3ds normally puts its first partition at 0x4000, so the NCCH header sits
// just past it. Fetching a little more than that identifies the format and its
// encryption in a single request instead of two.
#define ROMFORMAT_PROBE_BYTES 0x4400

// Inspect a ROM on the server. Returns false only on transport failure; an
// unrecognised layout still returns true with kind ROMFORMAT_UNKNOWN.
bool romformat_probe(int romId, const char *fsName, RomFormatInfo *out);

// One-line description for the ROM detail screen.
const char *romformat_describe(const RomFormatInfo *info);

// Whether this file could be installed as it stands.
bool romformat_is_installable(const RomFormatInfo *info);

#endif // ROMFORMAT_H
