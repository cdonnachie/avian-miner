# Licensing

avian-miner is available under the terms of the GNU General Public License
version 3. The full text is in [COPYING](../COPYING), which is what the
"see COPYING" notices in the upstream source files refer to, and in
[LICENSE](../LICENSE), which is the same text under the name GitHub and
packaging tools look for.

## Why version 3, when upstream is version 2

Not a choice:

- cpuminer-opt, which most of this miner is, is offered under "version 2 of
  the License, or (at your option) any later version". The later option is
  taken here.

- The CUDA and HIP kernels under `cuda/` derive from suprminer, which is GPL
  version 3 and carries Apache-2.0 material as well. Apache-2.0 can be
  conveyed under GPLv3 and cannot be conveyed under GPLv2, so a combined work
  including it has no version 2 form to distribute.

Upstream's GPLv2 text is kept as [COPYING.GPLv2](../COPYING.GPLv2), since the
files that carry a "version 2 or later" notice still carry it, and that notice
is what makes the upgrade above legitimate.
