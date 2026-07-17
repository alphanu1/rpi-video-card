# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
#
# Stable build environment for CRTPi. Debian 12's gcc/glibc are what
# Buildroot releases are tested against -- this permanently ends the
# rolling-distro host-toolchain whack-a-mole (gnulib vs GCC15/C23 glibc).
FROM debian:12-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc g++ make git unzip rsync bc wget cpio file \
    libssl-dev libncurses-dev python3 perl bzip2 xz-utils ca-certificates \
    locales && \
    rm -rf /var/lib/apt/lists/* && \
    sed -i 's/# en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen
ENV LANG=en_US.UTF-8 FORCE_UNSAFE_CONFIGURE=0
WORKDIR /work
