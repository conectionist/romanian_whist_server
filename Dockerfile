# syntax=docker/dockerfile:1

# Ubuntu 26.04 LTS in both stages, so the runtime has the libraries the build linked.
FROM ubuntu:26.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ccache \
        libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*
ENV CCACHE_DIR=/ccache
WORKDIR /src
COPY . .
# Two compile jobs unless told otherwise, so a build on a desktop leaves it usable.
ARG JOBS=2
# The build context has no .git, so the commit the server reports is passed in.
ARG WHIST_COMMIT=
RUN --mount=type=cache,target=/ccache \
    cmake --preset release -DWHIST_SERVER_BUILD_TESTS=OFF \
          -DWHIST_SERVER_COMMIT="${WHIST_COMMIT}" \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 && nice -n 19 cmake --build --preset release --target whist_server --parallel "${JOBS}" \
 && strip build-release/server/app/whist_server

FROM ubuntu:26.04
LABEL org.opencontainers.image.title="Romanian Whist server" \
      org.opencontainers.image.source="https://github.com/conectionist/romanian_whist_server" \
      org.opencontainers.image.licenses="AGPL-3.0-or-later"
RUN apt-get update \
 && apt-get install -y --no-install-recommends libjsoncpp26 libssl3t64 libuuid1 zlib1g \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --user-group --uid 10001 --no-create-home --home-dir /nonexistent \
            --shell /usr/sbin/nologin whist
COPY --from=build /src/build-release/server/app/whist_server /usr/local/bin/whist_server
COPY server/www /opt/whist/www
COPY deploy/docker-config.json /etc/whist/config.json
COPY LICENSE /usr/share/doc/romanian_whist_server/LICENSE
# Fails the build when the packages above miss a library the server links.
RUN ! ldd /usr/local/bin/whist_server | grep "not found"
USER whist
WORKDIR /opt/whist
EXPOSE 8080
STOPSIGNAL SIGTERM
# The server has no health route, so this checks that it accepts connections on its port.
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --start-interval=1s --retries=3 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080"]
ENTRYPOINT ["/usr/local/bin/whist_server"]
CMD ["--config", "/etc/whist/config.json"]
