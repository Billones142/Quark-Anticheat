# Fedora 44 build environment for RecoilEngine.
#
# Why Fedora 44 specifically: the test VM (Nobara Linux, Fedora-based) runs glibc 2.43.
# Binaries built on an Arch Linux host (glibc 2.44+) will not load on the VM — glibc is
# backwards- but not forwards-compatible. This image's glibc matches the VM's, so
# anything built here is a straight `scp` away from running there, and none of the
# compilation eats the VM's own limited CPU/RAM.
#
# See ../../docs/vm_test_environment.md section 6 for how this was derived and used.

FROM fedora:44

RUN dnf install -y \
        gcc-c++ \
        cmake \
        git \
        sdl2-compat-devel \
        DevIL-devel \
        freetype-devel \
        fontconfig-devel \
        libX11-devel \
        libXcursor-devel \
        expat-devel \
        openal-soft-devel \
        libvorbis-devel \
        libunwind-devel \
        zlib-ng-compat-devel \
        libglvnd-devel \
        libcurl-devel \
        procps-ng \
    && dnf clean all

WORKDIR /src
