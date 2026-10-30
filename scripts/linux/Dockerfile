# The build host for a portable Linux release: the oldest glibc worth
# supporting, with the CUDA toolkit already in it.
#
# The tarball built directly in WSL needs GLIBC_2.39, because that is what
# Ubuntu 24.04 has, and no mining distribution ships anything that new. The
# current HiveOS stable image, 0.6-230, is Ubuntu 22.04 jammy with glibc 2.35,
# so that is the floor set here. 22.04 is also a supported target for both the
# CUDA toolkit and ROCm, so one image can build every backend.
#
# Nvidia's devel image rather than ubuntu:22.04 plus a toolkit install: it is
# the same toolkit from the same vendor, pinned by tag, and it saves scripting
# a 3 GB download that would otherwise have to be got right here.
ARG CUDA_TAG=12.9.1-devel-ubuntu22.04
FROM nvidia/cuda:${CUDA_TAG}

# tzdata prompts without this, and a build must not stop to ask.
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential autoconf automake libtool pkg-config \
        git ca-certificates curl \
        libcurl4-openssl-dev libjansson-dev libgmp-dev \
    && rm -rf /var/lib/apt/lists/*

# Rust, because libavian-sv2 has to be built in here too. A .a compiled
# against a newer glibc carries its symbol versions into the miner and undoes
# the whole point of the image. Pinned rather than "stable" so the same commit
# gives the same binary next month, and pinned to what WSL has, so the library
# in a container release is the one the local release was already built with.
#
# Not the workspace's declared rust-version of 1.75: a transitive dependency
# in Cargo.lock is edition 2024, so anything before 1.85 fails to resolve at
# all with "the package requires the Cargo feature called edition2024".
ARG RUST_VERSION=1.98.0
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --profile minimal --default-toolchain ${RUST_VERSION} \
    && rustc --version && cargo --version

WORKDIR /build
