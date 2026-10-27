# chronos-serve, the simulator CLI and the built UI in one image: docker compose up, then http://localhost:8080.
# .git comes into the build stage so each binary records its commit and source digest, as a local build does.

FROM node:24-bookworm-slim AS web
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS engine
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++-14 cmake make patch git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
 && cmake --build build --parallel --target simulator chronos-serve chronos-e2e-fixture

FROM ubuntu:24.04
COPY --from=engine /src/build/simulator /src/build/chronos-serve /src/build/chronos-e2e-fixture /usr/local/bin/
COPY --from=web /src/web/dist /opt/chronos/web
COPY scripts/docker-entrypoint.sh /usr/local/bin/
# runs/, sessions/ and the per-symbol store live here: mount a volume to keep them.
WORKDIR /var/lib/chronos
EXPOSE 8080
ENTRYPOINT ["docker-entrypoint.sh"]
