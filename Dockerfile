FROM alpine:3.16
RUN apk add automake autoconf gcc libtool pkgconfig git musl-dev libusb-dev make bash
COPY . /openocd
WORKdir /openocd
RUN chmod +x bootstrap configure jimtcl/configure jimtcl/autosetup/autosetup-find-tclsh src/jtag/drivers/libjaylink/configure build-aux/install-sh 
RUN ./bootstrap
RUN ./configure --enable-dirtyjtag
RUN make -j10
RUN make install
