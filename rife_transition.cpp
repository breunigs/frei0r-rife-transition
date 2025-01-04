#include "frei0r.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <ftw.h>
#include <map>
#include <mutex>
#include <stdio.h>
#include <unistd.h>
#include <vector>

#include "cpu.h"
#include "gpu.h"
#include "platform.h"
#include "rife.h"

const double g_default_fps = 30000 / 1001;
const int g_default_fade_frames = 8;
const double g_default_duration = 1.0 / g_default_fps * g_default_fade_frames;

std::mutex g_mutex;
bool g_debug = false;

template <typename... Args>
void debug(Args... args) {
    if (g_debug) {
        std::ostringstream oss;
        oss << "DEBUG: ";
        (oss << ... << args);
        std::cerr << oss.str() << "\n";
    }
}

class Loader {
public:
    static Loader &getInstance() {
        static Loader instance;
        return instance;
    }

    int process(std::string model_path, int device, ncnn::Mat mat1, ncnn::Mat mat2, double ratio, ncnn::Mat oimg) {
        std::lock_guard<std::mutex> guard(g_mutex);
        auto model = get_or_init_rife(model_path, device);
        if (!model) return -31337;

        return model->process(mat1, mat2, ratio, oimg);
    }

    Loader(Loader const &) = delete;
    void operator=(Loader const &) = delete;

private:
    Loader() {}

    typedef std::tuple<std::string, int> rifeKey;
    std::map<rifeKey, RIFE *> m_rife_map;

    bool m_gpu_initialized = false;

    RIFE *get_or_init_rife(std::string model_path, int device) {
        rifeKey key = std::make_tuple(model_path, device);
        auto memoized = m_rife_map.find(key);
        if (memoized != m_rife_map.end()) {
            return memoized->second;
        }

        bool has_temp = false;
        if (model_path == "") {
            has_temp = true;
            model_path = write_embedded_model();
        }
        if (model_path == "") {
            std::cerr << "ERROR: No model specified and failed to write embedded model\n";
            return NULL;
        }

        if (model_path.find("rife-v4") == std::string::npos) {
            std::cerr << "ERROR: Model path (" << model_path
                      << ") must contain version somewhere in path name (e.g. /tmp/rife-v4.25/).\n";
            return NULL;
        }

        int padding = 32;
        if (model_path.find("rife-v4.25") != std::string::npos) padding = 64;
        if (model_path.find("rife-v4.25-lite") != std::string::npos) padding = 128;
        if (model_path.find("rife-v4.26") != std::string::npos) padding = 64;
        // TODO error handling if model unsupported?
        debug("padding=", padding);

        if (device != -1) initialize_gpu();

        if (device >= ncnn::get_gpu_count()) {
            std::cerr << "ERROR: cannot run on device=" << device << ", not that many GPUs detected.\n";
            ncnn::destroy_gpu_instance();
            return NULL;
        }
        debug("device=", device);

        RIFE *rife = new RIFE(device,
                              0 /* tta_mode */,
                              0 /* tta_temporal_mode */,
                              false /* uhd mode */,
                              1 /* num_threads */,
                              false /* rife_v2 */,
                              true /* rife_v4 */,
                              padding);
        debug("initialized RIFE");

        rife->load(model_path);
        debug("loaded model @ ", model_path);

        if (has_temp) nftw(model_path.c_str(), delete_file, 64, FTW_DEPTH | FTW_PHYS);

        m_rife_map[key] = rife;
        return rife;
    }

    // frei0r uninits only at the end of the filter pipeline, so without some
    // other hack like "ratio > 1.0", we cannot unload RIFE ahead of time. We'd
    // also need some kind of counter for the still active plugins to avoid
    // discarding it too early. Since this is effort, and we don't gain much,
    // uninit is not implemented at the moment.

    // void uninit_rife() {
    //     debug("destroying RIFE instance");
    //     // iterate over m_rife_map and clean it up?
    //     if (m_gpu_initialized) ncnn::destroy_gpu_instance();
    // }

    void initialize_gpu() {
        if (m_gpu_initialized) return;
        ncnn::create_gpu_instance();
        m_gpu_initialized = true;
    }

    std::string write_embedded_model() {
        std::string model_path = std::filesystem::temp_directory_path();
        model_path += "/@@EMBEDDED_MODEL_NAME@@";
        std::filesystem::create_directories(model_path);

        extern const char _binary_flownet_param_start, _binary_flownet_param_end;
        const char *flownet_param = &_binary_flownet_param_start;
        const size_t flownet_param_len = &_binary_flownet_param_end - &_binary_flownet_param_start;
        auto outfile = fopen((model_path + "/flownet.param").c_str(), "wb");
        if (outfile == NULL) {
            debug("failed writing model to dir", model_path);
            return "";
        }
        fwrite(flownet_param, 1, flownet_param_len, outfile);
        fclose(outfile);

        extern const char _binary_flownet_bin_start, _binary_flownet_bin_end;
        const char *flownet_bin = &_binary_flownet_bin_start;
        const size_t flownet_bin_len = &_binary_flownet_bin_end - &_binary_flownet_bin_start;
        outfile = fopen((model_path + "/flownet.bin").c_str(), "wb");
        if (outfile != NULL) {
            fwrite(flownet_bin, 1, flownet_bin_len, outfile);
            fclose(outfile);
        }

        debug("wrote embedded model to ", model_path);
        return model_path;
    }

    static int delete_file(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
        int rv = remove(fpath);
        if (rv) perror(fpath);
        return rv;
    }
};

class RifeTransition : public frei0r::mixer2 {
public:
    RifeTransition(unsigned int width, unsigned int height) {
        m_duration = g_default_duration;
        m_start = -1.0;
        m_size = width * height * sizeof(uint32_t);
        m_device = ncnn::get_default_gpu_index();
        m_model_path = "";
        register_param(m_duration,
                       "duration",
                       std::format("seconds; duration of the transition (i.e. the two videos overlapping). Default: {}",
                                   m_duration));
        register_param(m_model_path,
                       "model_path",
                       "Path to model directory with flownet.{bin,param} files. Default: @@EMBEDDED_MODEL_NAME@@");
        register_param(
            m_device,
            "device",
            std::format("select which GPU to use for calculations. cpu=-1 gpu0=0 gpu1=1 and so on. Default: gpu{}",
                        m_device));
        register_param(m_debug, "debug", "print verbose/debug information to stderr");
    }

    ~RifeTransition() {}

    virtual void update(double time_s, uint32_t *out, const uint32_t *in1, const uint32_t *in2) {
        g_debug = m_debug > 0.5;
        debug("update called for time=", time_s, "s");

        if (m_duration <= 0.0) {
            if (m_start < 0.0) {
                std::cerr << "WARNING: transition period should be greater than zero\n";
            } else {
                debug("duration is smaller than 0, copying second input");
            }
            memcpy(out, in2, m_size);
            return;
        }

        double ratio = m_offset_ratio;
        if (m_start < 0.0) {
            debug("setting m_start=", time_s, "s");
            m_start = time_s;
            m_count = 0;
        } else {
            ratio = (time_s - m_start) / m_duration + m_offset_ratio;
            if (ratio > 1.0) {
                debug("ratio=", ratio, " is beyond transition period, copying input2");
                debug("created ", m_count, " RIFE frames");
                memcpy(out, in2, m_size);
                return;
            }
        }
        debug("selecting ratio=", ratio);

        std::vector<uint8_t> rgb1 = rgba2rgb(in1, width, height);
        ncnn::Mat mat1 = ncnn::Mat(width, height, rgb1.data(), (size_t)3, 3);

        std::vector<uint8_t> rgb2 = rgba2rgb(in2, width, height);
        ncnn::Mat mat2 = ncnn::Mat(width, height, rgb2.data(), (size_t)3, 3);

        ncnn::Mat oimg = ncnn::Mat(width, height, (size_t)3, 3);

        int device = static_cast<int>(m_device);
        int ret = Loader::getInstance().process(m_model_path, device, mat1, mat2, ratio, oimg);
        if (ret < 0) {
            std::cerr << "WARNING: RIFE at ratio=" << ratio << "failed with ret=" << ret << "\n";
            memcpy(out, in2, m_size);
            return;
        }
        debug("rendered interpolation ratio=", ratio);
        m_count += 1;

        rgb2rgba((const uint8_t *)oimg.data, out, width, height);
    }

private:
    int m_count = 0;
    double m_duration;
    double m_device;
    double m_start;
    uint32_t m_size;
    std::string m_model_path;
    double m_debug = 0.0;
    // how much to offset the ratio to avoid the first frame transition being
    // wasted (i.e. ratio=0 → fully copy frame from video1)
    const double m_offset_ratio = 0.03;

    std::vector<uint8_t> rgba2rgb(const uint32_t *in, size_t width, size_t height) {
        std::vector<uint8_t> rgb_buffer(width * height * 3);
        uint8_t *out_ptr = rgb_buffer.data();

        for (size_t i = 0; i < width * height; ++i) {
            uint32_t pixel = in[i];
            *out_ptr++ = static_cast<uint8_t>(pixel);       // r
            *out_ptr++ = static_cast<uint8_t>(pixel >> 8);  // g
            *out_ptr++ = static_cast<uint8_t>(pixel >> 16); // b
        }
        return rgb_buffer;
    }

    void rgb2rgba(const uint8_t *rgb, uint32_t *out, size_t width, size_t height) {
        for (size_t i = 0; i < width * height; ++i) {
            uint8_t r = rgb[i * 3 + 0];
            uint8_t g = rgb[i * 3 + 1];
            uint8_t b = rgb[i * 3 + 2];
            out[i] = (0xFF << 24) | (b << 16) | (g << 8) | r; // Adding alpha channel (0xFF)
        }
    }
};

frei0r::construct<RifeTransition> plugin(
    "rife_transition", "Transition between two videos using RIFE", "Stefan Breunig", 0, 2, F0R_COLOR_MODEL_RGBA8888);
