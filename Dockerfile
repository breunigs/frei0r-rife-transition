FROM debian:testing-slim AS build
WORKDIR /src/

RUN apt-get update --yes \
  && apt-get install --yes --no-install-recommends \
  bash \
  ca-certificates \
  cmake \
  frei0r-plugins-dev \
  g++ \
  git \
  libvulkan-dev \
  libwebp-dev \
  make \
  && rm -rf /var/lib/apt/lists/*

COPY . /src/
RUN \
  --mount=type=cache,target=/src/build/ \
  --mount=type=cache,target=/src/rife-ncnn-vulkan/ \
  /src/build.sh

FROM scratch AS artifacts
COPY --from=build /src/rife_transition.so /
