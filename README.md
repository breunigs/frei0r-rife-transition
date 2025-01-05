## frei0r-rife-transitions

frei0r plugin wrapper around
[rife-ncnn-vulkan](https://github.com/TNTwise/rife-ncnn-vulkan/). It allows two
transition two videos using RIFE to calculate the intermediate images. This
often hides cuts very well.

Generally speaking, the RIFE algorithm is more forgiving if the camera path,
recording angle, or position ("translation") are not the same. Especially
when the camera is moving, RIFE makes it easier for the viewer to not get
disoriented, even when artifacts are visible. This somewhat also works for
camera movement speed changes, although the abrupt slow-down or speed-up
will be noticable.

Here's two examples comparing the RIFE results vs a classic crossfade transition.
The camera speed for each transition is roughly matched, but position, angle are
different.

- https://github.com/user-attachments/assets/afd1a0c0-e818-4d3b-8995-4df30ba76390

- https://github.com/user-attachments/assets/a932f69d-0e3f-480b-a4eb-e895d897cb25

- https://github.com/user-attachments/assets/8f8d536b-65b3-4c2e-9043-a18d22979287

- https://github.com/user-attachments/assets/690392d4-18c7-49a9-b1b0-8edb2fa9e6ab

Note that the transition section chosen for each pair of videos is "optimal".
Other sections differ more in angle, position and speed. Of course, this was
only eyeballed by n=2, and not for all possible combinations.

### Downloading

You can download a [release
version](https://github.com/breunigs/frei0r-rife-transition/releases/) or the
build artifact [attached to every
build](https://github.com/breunigs/frei0r-rife-transition/actions).
Currently only linux/amd64 artifacts are being built.

Place the `rife_transition.so` in a frei0r PATH, usually `~/.frei0r-1/lib/`.
Note that you'll most likely need a custom ffmpeg build, see [building
ffmpeg](#3-ffmpeg).

### Building

##### 1. Clone

```bash
git clone https://github.com/breunigs/frei0r-rife-transition
cd frei0r-rife-transition
```

#### 2a. Build locally

```bash
apt-get install bash cmake frei0r-plugins-dev g++ git libvulkan-dev libwebp-dev make
./build.sh
# or possibly ./install.sh
```

#### 2b. build in container

```bash
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
  -filter_complex '[0][1]frei0r=filter_name=rife_transition:filter_params=0.2669[out]' \
  -map '[out]' \
  -pix_fmt yuv420p \
  -f yuv4mpegpipe - \
| mpv -
```

The mixer immediately starts calculating intermediate images from the two inputs
using RIFE. In the beginning the frames are mostly from the first input. Towards
the end of the transition duration, they're mostly from the second input.
Afterwards it will solely return frames from the second input.


#### Complex Example

For performance reasons, it's probably beneficial to not run the filter longer
than the duration to avoid copying each frame. Here's a more complex example
which joins two videos with some time before and after. Note that the timestamps
within `-filter_complex` are relative to the offsets specified via `-ss`:

```bash
ffmpeg \
  -hwaccel auto \
  -loglevel verbose \
  -nostats \
  \
  -ss 00:00:03.000 \
  -to 00:00:04.000 \
  -i 'input1.mp4' \
  \
  -ss 00:00:02.000 \
  -to 00:00:03.000 \
  -i 'input2.mp4' \
  \
  -filter_complex '
    [0]segment=timestamps=0.68[prev0][trans0];
    [1]segment=timestamps=0.36[trans1][next1];

    [prev0]setpts=PTS-STARTPTS[prev0];
    [trans0]setpts=PTS-STARTPTS[trans0];
    [trans1]setpts=PTS-STARTPTS[trans1];
    [next1]setpts=PTS-STARTPTS[next1];

    [trans0][trans1]frei0r=filter_name=rife_transition:filter_params=0.32||0|1[trans01];

    [prev0][trans01][next1]concat=n=3[out]
  ' \
  -map '[out]' \
  -pix_fmt yuv420p \
  -r 25 \
  -f yuv4mpegpipe \
  - | \
  mpv \
  --pause \
  --no-terminal \
  -
```

In more detail, this is how the `-filter_complex` works:

1. Assuming input fps=25, then a frame is shown for 0.04s. Let's decide on a
   transition duration of around 0.32s.

   Both inputs are exactly 1s long, due to the `-ss` and `-to` flags. We need to
   split the first input from the end, i.e. 1s - 0.32s = 0.68s.

   ```
   [0]segment=timestamps=0.68[prev0][trans0];
   ```

2. Similarly, we want the first 0.32s of the second input, i.e. we split from
   the start. Due to the transition segments being taken once from the end (first
   input) and once from the start (second input) their duration is off-by-one
   frame. To combat that, there are three options:

   - shorten first's input transition segment duration by one frame: `0.68s +
     0.04s = 0.72s`
   - extend second's input transition segment duration by one frame: `0.32s +
     0.04s = 0.36s`
   - add `shortest=1` as a frei0r filter option and drop one frame

   ```
   [1]segment=timestamps=0.36[trans1][next1];
   ```

3. Reset all frame timestamps to start at 0 again. Otherwise the video will
   "hang" or "skip" parts because the timestamps are funky.

   ```
   [prev0]setpts=PTS-STARTPTS[prev0];
   [trans0]setpts=PTS-STARTPTS[trans0];
   [trans1]setpts=PTS-STARTPTS[trans1];
   [next1]setpts=PTS-STARTPTS[next1];
   ```

4. Transition for 0.32s, using the embedded model, on GPU0 and enable debug
   printout.

   ```
   [trans0][trans1]frei0r=filter_name=rife_transition:filter_params=0.32||0|1[trans01];
   ```

5. Concatenate all segments together for the final video.

   ```
   [prev0][trans01][next1]concat=n=3[out]
   ```

### Plugin configuration

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
* `debug`, bool, optional, set to `1` to print debug info to stderr
