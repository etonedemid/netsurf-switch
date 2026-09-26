# Reproducible cross-build environment for NetSurf on Nintendo Switch.
#   docker build -t netsurf-switch-build tools-switch
#   docker run --rm -v "$PWD":/src/netsurf-switch netsurf-switch-build
FROM devkitpro/devkita64:latest

RUN sed -i 's#http://deb.debian.org#https://deb.debian.org#' /etc/apt/sources.list.d/* /etc/apt/sources.list 2>/dev/null; \
    rm -f /etc/apt/sources.list.d/devkitpro.list; \
    apt-get update && apt-get install -y --no-install-recommends \
        build-essential flex bison gperf git xxd zlib1g-dev libpng-dev libjpeg-dev && rm -rf /var/lib/apt/lists/*

ENV NSPORT_ROOT=/src
COPY build-prefix.sh /usr/local/bin/build-prefix.sh
RUN /usr/local/bin/build-prefix.sh /src/prefix-switch

WORKDIR /src/netsurf-switch
CMD ["./tools-switch/build.sh"]
