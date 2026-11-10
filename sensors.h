/*
 * Temperature, power and clock readings for the API.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * Every reading here is optional. A value that is not available is negative
 * (or NULL, for the source name), and the API leaves it out rather than
 * writing a zero that reads as a real temperature. Nothing in this file
 * knows about threads or cards: callers hand it a PCIe bus number or a
 * logical CPU and get back whatever the platform will say about it.
 */

#ifndef SENSORS_H__
#define SENSORS_H__

#include <stdbool.h>

struct gpu_sensors
{
   float temp;        /* core, degrees C */
   float mem_temp;    /* memory, where the board has the sensor */
   float hotspot;     /* junction; AMD on Linux reports it, NVML does not */
   int   fan;         /* percent */
   float power;       /* watts, the whole board */
   int   clock;       /* core MHz */
   int   mem_clock;   /* memory MHz */
};

#define CPU_SENSOR_ZONES 8

struct cpu_sensors
{
   const char *source;   /* what answered: "k10temp", "LibreHardwareMonitor"... */
   float       temp;     /* the package, or Tctl/Tdie on AMD */
   float       power;    /* package watts */
   int         nzones;   /* per-die temperatures, CCDs on AMD */
   struct
   {
      char  label[16];
      float temp;
   } zone[ CPU_SENSOR_ZONES ];
};

/* Readings for the card on this PCIe bus. Returns true if any field is set.
 * Nvidia through NVML, loaded at run time if the driver is installed; AMD on
 * Linux through the hwmon node under the card's PCI device. */
bool sensors_gpu( int pci_bus, struct gpu_sensors *out );

/* The CPU package. Linux reads the k10temp, zenpower or coretemp driver and
 * RAPL for power. Windows has no user-mode source for either and this does
 * not install a driver to make one, so there it reads LibreHardwareMonitor's
 * WMI namespace when that program is running and reports nothing when it is
 * not. */
void sensors_cpu( struct cpu_sensors *out );

/* The effective clock of one logical CPU in MHz, or -1. On Windows this is
 * the performance counter the kernel derives from APERF/MPERF, so it is the
 * real average frequency over the last sample, boost included. */
int sensors_cpu_clock( int logical );

/* One line each time a source is found or found missing, so a rig with no
 * temperatures can read why in its log. Info is for what was found and for
 * an absence the user can act on; debug is for the rest. */
#define SENSORS_LOG_INFO  0
#define SENSORS_LOG_DEBUG 1
void sensors_set_log( void ( *fn )( int level, const char *line ) );

#endif
