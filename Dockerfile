FROM alpine:3.12 as builder
RUN apk add boost-dev gcc g++ git libaio-dev make ncurses-dev numactl-dev bash libexecinfo-dev
RUN cd /root && git clone https://github.com/breuner/elbencho.git && \
    cd elbencho && \
    NO_BACKTRACE=1 make -j "$(nproc)" && \
    make install
FROM alpine:3.12
COPY --from=builder /usr/local/bin/elbencho /usr/local/bin
RUN apk add boost-regex boost-program_options libaio-dev ncurses-dev numactl-dev libexecinfo-dev

ENTRYPOINT ["/usr/local/bin/elbencho"]
