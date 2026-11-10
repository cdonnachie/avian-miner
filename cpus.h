/*
 * Which CPUs the miner may pin threads to, and how the machine is laid out.
 *
 * Copyright 2026 The avian-miner developers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * --cpu-affinity used to be a hex mask and nothing else, which capped it at
 * 64 CPUs and made running two miners on one machine an exercise in
 * arithmetic that went wrong silently: ask for more threads than the mask
 * has CPUs and the extra ones wrapped round onto cores already taken. This
 * takes a list as well, 0-21 or 0-11,16-23, keeps the hex form for the
 * flight sheets that have it, and can say how the CPUs are grouped into
 * cores, which is what deciding the split actually needs.
 */
#ifndef CPUS_H
#define CPUS_H

#include <stdbool.h>
#include <stddef.h>

/* The most logical CPUs this keeps track of. Threadripper tops out at 192
 * today; a machine beyond this still mines, on its first thousand. */
#define CPUS_MAX 1024

/* Read a --cpu-affinity argument into the set: "0x3fffff" is a mask, bit n
 * for CPU n, as it always was; anything else is a list of CPUs and ranges,
 * "0-21" or "0-11,16-23". Every CPU named has to exist on this machine,
 * which has num_cpus of them. On failure err holds a sentence saying why
 * and the set is left as it was. */
bool cpus_parse( const char *arg, int num_cpus, char *err, size_t err_len );

/* Whether thread may be pinned to cpu. Every CPU, until cpus_parse says
 * otherwise. */
bool cpus_allowed( int cpu );

/* How many of the first num_cpus are allowed. */
int cpus_count( int num_cpus );

/* The set drawn one character per CPU, '!' for allowed and '.' for not, as
 * the start-up line has always shown it. out gets at most n - 1 of them. */
void cpus_format( char *out, size_t n, int num_cpus );

/* The set as the shortest list, "0-20" or "0-11,16-23", the form
 * --cpu-affinity takes, so what the API reports can be pasted straight
 * back into a flight sheet. */
void cpus_ranges( char *out, size_t n, int num_cpus );

/* How many cores the first num_cpus logical CPUs are on, or 0 when how
 * they pair could not be read. */
int cpus_cores( int num_cpus );

/* --list-cpus: print how the CPUs group into cores and the --cpu-affinity
 * pair that runs a CPU miner and a GPU miner side by side on this machine.
 * Returns the process exit status. */
int cpus_list( int num_cpus );

#endif
