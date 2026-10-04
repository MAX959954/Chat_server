# syntax=docker/dockerfile:1
#
#   docker build -t chat_server .
#   docker run -d --name chat -p 65001:65001 chat_server
#   docker exec -it chat chat_client          # or: nc localhost 65001
#
# The build stage compiles and runs the whole test suite (unit + integration),
# so an image only exists if every test passed.

# ---- build + test ----
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev cmake make python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ---- runtime: just the binaries ----
FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --no-create-home chat
COPY --from=build /src/build/chat_server /src/build/chat_client /usr/local/bin/
USER chat
EXPOSE 65001
# chat_server handles SIGTERM itself (self-pipe, graceful shutdown), so
# `docker stop` notifies clients and exits 0 instead of waiting 10 s for SIGKILL.
# Extra flags can be appended: docker run chat_server -m 5000 -r 10
# Connections through a published port arrive from the Docker gateway, not
# localhost, so /shutdown from outside needs -e CHAT_ADMIN_PASSWORD=...
ENTRYPOINT ["chat_server"]
