FROM centos:8.3.2011 as builder

RUN yum install -y boost-devel gcc-c++ git libaio-devel make ncurses-devel numactl-devel


RUN cd /root && git clone https://github.com/breuner/elbencho.git && \
    cd elbencho && \
    make -j "$(nproc)" && \
    make install 

# multistage build..to trim down what we need, without needing --squash (which doesn't work
# with docker-hub autobuilder


FROM centos:8.3.2011
# the yum erase stuff is mainly for things that get included by default in this centos image. there may be
# more i can trim...but for now this is fine.
RUN yum install -y boost-system boost-thread boost-program-options libaio-devel ncurses-devel numactl-devel && \
    yum erase -y xz vim-minimal procps-ng ethtool iputils glibc-devel binutils tar isl shadow-utils iproute bind-export-libs cpio libicu-devel findutils cpp &&\
    yum clean all

# we only want the main binary, not the -unstripped one (too big :)

COPY --from=builder /root/elbencho/bin/elbencho /usr/local/bin 

ENTRYPOINT ["/usr/local/bin/elbencho"]
