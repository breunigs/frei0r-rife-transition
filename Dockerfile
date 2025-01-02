FROM debian:testing-slim AS build
WORKDIR /build/

RUN apt-get update --yes \
  && apt-get install --yes --no-install-recommends \
  bash \
  cmake \
  frei0r-plugins-dev \
  g++ \
  git \
  libvulkan-dev \
  libwebp-dev \
  make \
  && rm -rf /var/lib/apt/lists/*

COPY . /build/
RUN ls -alh /build/ && ./build.sh

FROM scratch AS artifacts
COPY --from=build /build/rife_transition.so /
