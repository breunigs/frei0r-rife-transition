#include "frei0r.hpp"

#include <algorithm>
#include <filesystem>
#include <ftw.h>
#include <format>
#include <stdio.h>
#include <unistd.h>
#include <vector>

#include "cpu.h"
#include "gpu.h"
#include "platform.h"
#include "rife.h"

const double defaultFPS = 30000 / 1001;
const int defaultFadeFrames = 8;
const double defaultDuration = 1.0 / defaultFPS * defaultFadeFrames;

class RifeTransition : public frei0r::mixer2 {
public:
    RifeTransition(unsigned int width, unsigned int height) {
        m_duration = defaultDuration;
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

    ~RifeTransition() { uninit_rife(); }

    virtual void update(double time_s, uint32_t *out, const uint32_t *in1, const uint32_t *in2) {
        debug("update called for time=", time_s, "s");

        if (m_duration <= 0.0) {
            debug("duration is smaller than 0, copying second input");
            memcpy(out, in2, m_size);
            return;
        }

        double ratio = 0.0;
        if (m_start < 0.0) {
            debug("setting m_start=", time_s, "s");
            m_start = time_s;
        } else {
            ratio = (time_s - m_start) / m_duration;
            if (ratio > 1.0) {
                debug("ratio=", ratio, " is beyond transition period, copying input2");
                memcpy(out, in2, m_size);
                return;
            }
        }

        ratio = std::max(ratio, m_bump_extremes);
        ratio = std::min(ratio, 1.0 - m_bump_extremes);
        debug("ratio=", ratio);

        init_rife();
        if (!m_loaded) {
            debug("RIFE model not loaded, falling back to memcpy");
            memcpy(out, in2, m_size);
            return;
        }

        std::vector<uint8_t> rgb1 = rgba2rgb(in1, width, height);
        ncnn::Mat mat1 = ncnn::Mat(width, height, rgb1.data(), (size_t)3, 3);

        std::vector<uint8_t> rgb2 = rgba2rgb(in2, width, height);
        ncnn::Mat mat2 = ncnn::Mat(width, height, rgb2.data(), (size_t)3, 3);

        ncnn::Mat oimg = ncnn::Mat(width, height, (size_t)3, 3);

        int ret = m_rife->process(mat1, mat2, ratio, oimg);
        if (ret < 0) {
            std::cerr << "WARNING: RIFE at ratio=" << ratio << "failed with ret=" << ret << "\n";
            memcpy(out, in2, m_size);
            return;
        }
        debug("rendered interpolation ratio=", ratio);

        rgb2rgba((const uint8_t *)oimg.data, out, width, height);
    }

private:
    double m_duration;
    double m_device;
    double m_start;
    uint32_t m_size;
    std::string m_model_path;
    bool m_loaded = false;
    double m_debug = 0.0;
    const double m_bump_extremes = 0.05;
    int padding;
    RIFE *m_rife;

    template <typename... Args>
    void debug(Args... args) {
        if (m_debug > 0.5) {
            std::ostringstream oss;
            oss << "DEBUG: ";
            (oss << ... << args);
            std::cerr << oss.str() << "\n";
        }
    }

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

    void init_rife() {
        if (m_loaded) return;

        bool has_temp = (m_model_path == "") ? write_embedded_model() : false;

        if (m_model_path.find("rife-v4") == std::string::npos) {
            std::cerr << "ERROR: Model path (" << m_model_path
                      << ") must contain version somewhere in path name (e.g. /tmp/rife-v4.25/).\n";
            return;
        }

        padding = 32;
        if (m_model_path.find("rife-v4.25") != std::string::npos) padding = 64;
        if (m_model_path.find("rife-v4.25-lite") != std::string::npos) padding = 128;
        if (m_model_path.find("rife-v4.26") != std::string::npos) padding = 64;
        // TODO error handling if model unsupported?
        debug("padding=", padding);

        int device = static_cast<int>(m_device);
        if (device != -1) ncnn::create_gpu_instance();

        if (device >= ncnn::get_gpu_count()) {
            std::cerr << "ERROR: cannot run on device=" << device << ", not that many GPUs detected.\n";
            ncnn::destroy_gpu_instance();
            return;
        }
        debug("device=", device);

        if (m_duration <= 0.0) std::cerr << "WARNING: transition period should be greater than zero\n";
        debug("transition duration=", m_duration, "s");

        m_loaded = true;
        m_rife = new RIFE(device,
                          0 /* tta_mode */,
                          0 /* tta_temporal_mode */,
                          false /* uhd mode */,
                          1 /* num_threads */,
                          false /* rife_v2 */,
                          true /* rife_v4 */,
                          padding);
        debug("initialized RIFE");

        m_rife->load(m_model_path);
        debug("loaded model @ ", m_model_path);

        if (has_temp) nftw(m_model_path.c_str(), delete_file, 64, FTW_DEPTH | FTW_PHYS);
    }

    void uninit_rife() {
        if (!m_loaded) return;
        m_loaded = false;
        if (!m_rife) return;

        debug("destroying RIFE instance");

        delete m_rife;
        if (static_cast<int>(m_device) != -1) ncnn::destroy_gpu_instance();
    }

    bool write_embedded_model() {
        m_model_path = std::filesystem::temp_directory_path();
        m_model_path += "/@@EMBEDDED_MODEL_NAME@@";
        std::filesystem::create_directories(m_model_path);

        extern const char _binary_flownet_param_start, _binary_flownet_param_end;
        const char *flownet_param = &_binary_flownet_param_start;
        const size_t flownet_param_len = &_binary_flownet_param_end - &_binary_flownet_param_start;
        auto outfile = fopen((m_model_path + "/flownet.param").c_str(), "wb");
        if (outfile != NULL) {
            fwrite(flownet_param, 1, flownet_param_len, outfile);
            fclose(outfile);
        }

        extern const char _binary_flownet_bin_start, _binary_flownet_bin_end;
        const char *flownet_bin = &_binary_flownet_bin_start;
        const size_t flownet_bin_len = &_binary_flownet_bin_end - &_binary_flownet_bin_start;
        outfile = fopen((m_model_path + "/flownet.bin").c_str(), "wb");
        if (outfile != NULL) {
            fwrite(flownet_bin, 1, flownet_bin_len, outfile);
            fclose(outfile);
        }

        debug("wrote embedded model to ", m_model_path);
        return true;
    }

    static int delete_file(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
        int rv = remove(fpath);
        if (rv) perror(fpath);
        return rv;
    }
};

frei0r::construct<RifeTransition> plugin(
    "rife_transition", "Transition between two videos using RIFE", "Stefan Breunig", 0, 2, F0R_COLOR_MODEL_RGBA8888);
