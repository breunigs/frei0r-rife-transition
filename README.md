## frei0r-rife-transitions

frei0r plugin wrapper around
[rife-ncnn-vulkan](https://github.com/TNTwise/rife-ncnn-vulkan/). It allows two
transition two videos using RIFE to calculate the intermediate images. This
often hides cuts very well.


### Building

##### 1. Clone

```bash
git clone https://github.com/breunigs/frei0r-rife-transition
cd frei0r-rife-transition
```

#### 2a. Build locally

```bash
apt-get install cmake binutils git bash
./build.sh
# or possibly ./install.sh
```

#### 2b. build in container

```bash
# submodules are automatically cloned within Docker, but this caches them
# outside on the host.
git submodule update --init --recursive --depth 0
DOCKER_BUILDKIT=1 docker build -o type=local,dest=. --target artifacts .
```

#### 3. ffmpeg

As of writing, upstream ffmpeg doesn't support frei0r mixer plugins. You'll need
to build it yourself until the patch is merged upstream. See [ffmpeg's
compilation guide](https://trac.ffmpeg.org/wiki/CompilationGuide) for more
information.

```bash
git clone -b frei0r-mixers https://github.com/breunigs/ffmpeg/
cd ffmpeg
./configure --enable-frei0r --enable-gpl --enable-libx264
make -j
```

### Usage

```bash
ffmpeg \
  -i 'A.mp4' \
   -i 'B.mp4' \
   -filter_complex '[0][1]frei0r=filter_name=rife_transition:filter_params=0.2669:inputs=2[out]' \
   -map '[out]' \
   -pix_fmt yuv420p \
   -f yuv4mpegpipe - \
| mpv -
```

The mixer immediately starts calculating intermediate images from the two inputs
using RIFE. In the beginning the frames are mostly from the first input. Towards
the end of the transition duration, they're mostly from the second input.
Afterwards it will solely return frames from the second input.

For performance reasons, it's probably beneficial to not run the filter longer
than the duration to avoid copying each frame.

The filter is configured like any other frei0r filter. See [upstream ffmpeg
filter documentation](https://ffmpeg.org/ffmpeg-filters.html#frei0r-1) for more
details.

The plugin accepts these parameters:

* `duration`, double, required, time of the transitions in seconds.
* `model_path`, string, optional, path to the RIFE model to use. You can grab
  recent versions from [TNTwise's
  fork](https://github.com/TNTwise/rife-ncnn-vulkan/tree/master/models). Note
  that the plugin only supports RIFE v4+ and was only tested up to v4.26. The
  plugin embeds a usable version, so specifying the model directory is optional.
* `device`, integer, optional, specifies which GPU to use, where the first GPU
  is `0`, the second `1` and so on. CPU is `-1`, but the results were broken on
  my machine.
