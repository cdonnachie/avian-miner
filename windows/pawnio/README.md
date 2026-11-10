# PawnIO modules for avian-sensors

`avian-sensors.exe` reads the CPU's registers through PawnIO, which runs
only signed modules. Two of them are needed, one per vendor:

| file | reads |
| --- | --- |
| `AMDFamily17.bin` | Zen, family 17h to 1Ah: SMN temperatures and the energy MSRs |
| `IntelMSR.bin` | Intel Core, family 6: thermal status, TjMax and the RAPL MSRs |

They are the LGPL-2.1 modules from
<https://github.com/namazso/PawnIO.Modules>, and the signed builds are on
that repository's Releases page as one zip. `scripts/windows/fetch-pawnio-modules.sh`
downloads the release it is pinned to, checks the zip against the SHA-256
recorded in the script, and puts the two files here; `win-package.sh` runs
it when they are missing, so a normal release build needs nothing done by
hand. A zip built without them still works, and the service says which
file it is missing in its log.

They are not committed here because they are other people's signed
binaries: the driver checks the signature and refuses an altered module, so
the right copy is always the published one. `.gitignore` keeps `*.bin` out
of this directory. LibreHardwareMonitor embeds the same builds in
`LibreHardwareMonitorLib.dll`, which is another place to get them from if
the releases page is unreachable.
