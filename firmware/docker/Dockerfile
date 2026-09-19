FROM ubuntu:18.04
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        make gcc bison flex fakeroot gettext bc libncurses5-dev u-boot-tools gperf \
        tar gzip unzip file rsync python3 gosu ca-certificates liblzo2-2 && \
    rm -rf /var/lib/apt/lists/*
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
COPY scripts/sdk-init.sh /usr/local/bin/sdk-init.sh
COPY scripts/sdk-build.sh /usr/local/bin/sdk-build.sh
COPY scripts/sdk-export.sh /usr/local/bin/sdk-export.sh
COPY scripts/sdk-clean.sh /usr/local/bin/sdk-clean.sh
RUN chmod +x /usr/local/bin/*.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
