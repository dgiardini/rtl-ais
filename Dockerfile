# -------------------
# The build container
# -------------------
FROM debian:bookworm-slim AS build

WORKDIR /usr/src/app

COPY . /usr/src/app

# Upgrade bookworm and install dependencies
RUN apt-get -y update && apt -y upgrade && apt-get -y install --no-install-recommends \
    rtl-sdr \
    librtlsdr-dev \
    libusb-1.0-0-dev \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Build rtl_ais
RUN make && \
    make install


# -------------------------
# The application container
# -------------------------
FROM debian:bookworm-slim

LABEL org.opencontainers.image.title="rtl-ais"
LABEL org.opencontainers.image.description="AIS decoding using RTL-SDR dongle"
LABEL org.opencontainers.image.authors="Bryan Klofas KF6ZEO bklofas@gmail"
LABEL org.opencontainers.image.source="https://github.com/bklofas/rtl-ais"

# Upgrade bookworm and install dependencies
RUN apt-get -y update && apt -y upgrade && apt-get -y install --no-install-recommends \
    tini \
    rtl-sdr \
    librtlsdr-dev \
    libusb-1.0-0-dev &&\
    rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/src/app /

# Use tini as init.
ENTRYPOINT ["/usr/bin/tini", "--"]

CMD ["/rtl_ais", "-n"]

