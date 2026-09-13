FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make libsqlite3-dev libssl-dev ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Pinned single-header HTTP library.
RUN curl -fsSL https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h \
    -o src/httplib.h

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j2

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 libssl3 ca-certificates ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/build/tubelite /app/tubelite
COPY public /app/public
COPY data /app/data

ENV PORT=10000
ENV DATA_DIR=/app/data
EXPOSE 10000

CMD ["/app/tubelite"]
