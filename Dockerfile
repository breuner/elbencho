FROM centos:8.3.2011 

RUN yum install -y boost-devel gcc-c++ git libaio-devel make ncurses-devel numactl-devel rpm-build 


RUN cd /root && git clone https://github.com/breuner/elbencho.git && \
    cd elbencho && \
    make -j4 && \
    make rpm && \
    make install 
#get rid of stuff we don't need

RUN yum erase -y gcc-c++ git make rpm-build



#now run a sample test, should just take a few seconds.

ENTRYPOINT ["/usr/local/bin/elbencho"]



