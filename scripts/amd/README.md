# Running the AMD build on a rented GPU

Everything needed to go from a bare Ubuntu GPU instance to a verified HIP
build. Written for a DigitalOcean MI325X droplet, Ubuntu 26.04, but nothing
here is specific to that beyond the repository names.

| script | what it does | where it runs |
| --- | --- | --- |
| `root-setup.sh` | docker, group membership, the ROCm image | on the host, as root, **once** |
| `build.sh` | configure `--with-hip` and build, in the container | on the host, as the ordinary user |
| `first.sh` + `first-stage-probe.sh` | every algorithm's 80-byte kernel against the CPU | same |
| `verify.sh` | the full sixteen-stage chain, per forced order | same |

## The one thing the ROCm documentation does not tell you

The kernel driver and the ROCm userspace come from **different repositories**,
and ROCm's install page covers only the second. On a stock Ubuntu image the
card is present on the bus with nothing bound to it, and `rocminfo` fails with
`Unable to open /dev/kfd: Invalid argument`, which looks like a permissions
problem and is not one. Check it with

```sh
ls -l /sys/bus/pci/devices/0000:*/driver   # should point at amdgpu
```

The driver comes from `repo.radeon.com/amdgpu`, and the suite codename for
Ubuntu 26.04 is `resolute`:

```sh
sudo mkdir --parents --mode=0755 /etc/apt/keyrings
wget https://repo.radeon.com/rocm/rocm.gpg.key -O - | \
  gpg --dearmor | sudo tee /etc/apt/keyrings/rocm.gpg > /dev/null
echo 'deb [arch=amd64 signed-by=/etc/apt/keyrings/rocm.gpg] https://repo.radeon.com/amdgpu/31.60/ubuntu resolute main' \
  | sudo tee /etc/apt/sources.list.d/amdgpu.list
sudo apt update && sudo apt install -y amdgpu-dkms && sudo reboot
```

Install **only** the driver on the host. The ROCm userspace comes with the
container, which keeps the host's distribution out of ROCm's version matrix
entirely. On a 7.0 kernel the DKMS build is the one step that can fail; its
log is `/var/lib/dkms/amdgpu/*/build/make.log`.

## Then

```sh
scp -r <this tree> user@host:~/src/cpuminer-avian
bash scripts/amd/build.sh
bash scripts/amd/first.sh                                   # the 16 _80 kernels
ORDERS="1F80675605446D40 542CB1E146A7D915" bash scripts/amd/verify.sh
```

`build.sh` deletes the kernel objects before every build on purpose. The
`.cu.o` suffix rule carries no header dependency tracking, so editing anything
under `cuda/upstream` otherwise leaves stale objects in place and `make`
reports success having rebuilt nothing. That cost a correct fix being measured
as having no effect. It applies to the CUDA build too.
