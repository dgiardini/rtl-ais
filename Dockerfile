
FROM debian:bookworm-slim
LABEL "name"="rtl-ais" \
  "description"="AIS ship decoding using an RTL-SDR dongle" \
  "author"="Bryan Klofas KF6ZEO"

ENV APP=/usr/src/app

WORKDIR $APP

COPY . $APP

RUN apt-get -y update && apt -y upgrade && apt-get -y install --no-install-recommends \
  rtl-sdr \
  librtlsdr-dev \
  libusb-1.0-0-dev \
  make \
  build-essential \
  pkg-config \
  && rm -rf /var/lib/apt/lists/*
  
RUN make && \
  make install

CMD ["/usr/src/app/rtl_ais", "-n"]

