/*
 * The snapshot avian-sensors hands out, shared between the service that
 * writes it and the miner that reads it.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * Windows gives an ordinary process no way to read a CPU's temperature or
 * power; both live in registers only the kernel can touch. avian-sensors is
 * a small service that reads them through PawnIO, the signed driver
 * LibreHardwareMonitor uses, once a second, and hands the latest reading to
 * whoever connects to its named pipe. The miner stays unelevated and opens
 * the pipe like a file; if there is no pipe there is no reading, and it
 * mines as before.
 *
 * The protocol is one read: connect, receive this struct, done. Fixed size,
 * a magic and a version in front, so a miner and a service from different
 * releases either agree or notice.
 */
#ifndef AVIAN_SENSORS_H
#define AVIAN_SENSORS_H

#include <stdint.h>

#define AVSENS_PIPE     "\\\\.\\pipe\\avian-sensors"
#define AVSENS_MAGIC    0x534e5641u   /* "AVNS" */
#define AVSENS_VERSION  1
#define AVSENS_ZONES    8

/* Every reading is negative when it is not known. */
struct avsens_snapshot
{
   uint32_t magic;
   uint32_t version;
   uint32_t size;         /* sizeof this struct, as the writer had it */
   uint32_t reserved;
   uint64_t sampled_ms;   /* GetTickCount64() when the readings were taken */
   char     source[32];   /* "pawnio/amd", "pawnio/intel", "fake" */
   char     cpu[48];      /* the CPU's brand string, trimmed */
   float    temp;         /* package: Tctl/Tdie on AMD, package on Intel, C */
   float    power;        /* package watts over the last second or so */
   uint32_t nzones;
   struct { char label[16]; float temp; } zone[ AVSENS_ZONES ];
};

#endif
