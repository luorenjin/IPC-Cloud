FROM ubuntu:18.04
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        make gcc bison flex fakeroot gettext bc libncurses5-dev u-boot-tools gperf \
        tar gzip unzip file rsync python3 gosu ca-certificates liblzo2-2 curl && \
    rm -rf /var/lib/apt/lists/*

# 仓库 CMakeLists 要求 >= 3.16，而 ubuntu:18.04 的 apt cmake 仅 3.10，故装官方二进制
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.27.9/cmake-3.27.9-linux-x86_64.tar.gz \
      -o /tmp/cmake.tar.gz \
 && tar -xzf /tmp/cmake.tar.gz -C /opt \
 && ln -sf /opt/cmake-3.27.9-linux-x86_64/bin/cmake /usr/local/bin/cmake \
 && rm /tmp/cmake.tar.gz

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
COPY scripts/sdk-init.sh /usr/local/bin/sdk-init.sh
COPY scripts/sdk-build.sh /usr/local/bin/sdk-build.sh
COPY scripts/sdk-export.sh /usr/local/bin/sdk-export.sh
COPY scripts/sdk-clean.sh /usr/local/bin/sdk-clean.sh
RUN chmod +x /usr/local/bin/*.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
