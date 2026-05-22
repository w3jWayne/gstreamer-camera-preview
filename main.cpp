#include <gst/gst.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include <sys/stat.h>

class FakeCameraApp {
public:
    enum class SourceMode {
        kFake,
        kV4L2,
    };

    enum class VideoFormat {
        kMjpeg,
        kYuyv,
    };

    struct Options {
        SourceMode source = SourceMode::kFake;
        std::string device = "/dev/video0";
        int width = 1280;
        int height = 720;
        VideoFormat format = VideoFormat::kMjpeg;
        std::string capture_path;
    };

    FakeCameraApp() = default;
    ~FakeCameraApp() { stop(); }

    FakeCameraApp(const FakeCameraApp&) = delete;
    FakeCameraApp& operator=(const FakeCameraApp&) = delete;

    bool init(int argc, char** argv) {
        gst_init(&argc, &argv);
        return parse_args(argc, argv);
    }

    bool build_pipeline() {
        log_configuration();

        pipeline_ = gst_pipeline_new("camera_app");
        if (pipeline_ == nullptr) {
            log_error("failed to create pipeline");
            return false;
        }

        // Capture mode builds a one-shot file pipeline.
        if (capture_mode()) {
            return build_capture_pipeline();
        }

        if (options_.source == SourceMode::kFake) {
            return build_fake_pipeline();
        }

        return build_v4l2_pipeline();
    }

    bool start() {
        if (pipeline_ == nullptr) {
            log_error("pipeline is not built");
            return false;
        }

        const GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            log_error("failed to set pipeline state to PLAYING");
            return false;
        }

        const GstStateChangeReturn state_ret =
            gst_element_get_state(pipeline_,
                                  nullptr,
                                  nullptr,
                                  5 * GST_SECOND);
        if (state_ret == GST_STATE_CHANGE_FAILURE) {
            log_error("failed to confirm pipeline state change");
            return false;
        }

        started_ = true;
        std::cout << "pipeline started" << std::endl;
        if (!capture_mode()) {
            log_negotiated_resolution();
        }
        return true;
    }

    void run() {
        if (bus_ == nullptr) {
            return;
        }

        bool running = true;

        // Poll the pipeline bus for EOS and error messages.
        while (running) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus_,

                GST_CLOCK_TIME_NONE,

                static_cast<GstMessageType>(
                    GST_MESSAGE_ERROR | GST_MESSAGE_EOS)
            );

            if (msg == nullptr) {
                continue;
            }

            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                handle_error(msg);
                running = false;
                break;
            case GST_MESSAGE_EOS:
                if (capture_mode()) {
                    std::cout << "capture completed" << std::endl;
                } else {
                    std::cout << "received EOS" << std::endl;
                }
                running = false;
                break;
            default:
                break;
            }

            gst_message_unref(msg);
        }
    }

    void stop() {
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            if (started_) {
                std::cout << "pipeline stopped" << std::endl;
            }
        }

        if (bus_ != nullptr) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }

        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }

        source_ = nullptr;
        capsfilter_ = nullptr;
        decoder_ = nullptr;
        convert_ = nullptr;
        encoder_ = nullptr;
        filesink_ = nullptr;
        sink_ = nullptr;
        started_ = false;
    }

private:
    bool parse_args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "--help") {
                print_usage(argv[0]);
                return false;
            }

            if (arg == "--source") {
                if (!require_value(argc, i, "--source")) {
                    return false;
                }
                const std::string value = argv[++i];
                if (value == "fake") {
                    options_.source = SourceMode::kFake;
                } else if (value == "v4l2") {
                    options_.source = SourceMode::kV4L2;
                } else {
                    log_error("unsupported source: " + value);
                    return false;
                }
                continue;
            }

            if (arg == "--device") {
                if (!require_value(argc, i, "--device")) {
                    return false;
                }
                options_.device = argv[++i];
                continue;
            }

            if (arg == "--width") {
                if (!require_value(argc, i, "--width")) {
                    return false;
                }
                if (!parse_int(argv[++i], options_.width) || options_.width <= 0) {
                    log_error("invalid width");
                    return false;
                }
                continue;
            }

            if (arg == "--height") {
                if (!require_value(argc, i, "--height")) {
                    return false;
                }
                if (!parse_int(argv[++i], options_.height) || options_.height <= 0) {
                    log_error("invalid height");
                    return false;
                }
                continue;
            }

            if (arg == "--format") {
                if (!require_value(argc, i, "--format")) {
                    return false;
                }
                const std::string value = argv[++i];
                if (value == "mjpeg") {
                    options_.format = VideoFormat::kMjpeg;
                } else if (value == "yuyv") {
                    options_.format = VideoFormat::kYuyv;
                } else {
                    log_error("unsupported format: " + value);
                    return false;
                }
                continue;
            }

            if (arg == "--capture") {
                if (!require_value(argc, i, "--capture")) {
                    return false;
                }
                options_.capture_path = argv[++i];
                continue;
            }

            log_error("unknown argument: " + arg);
            print_usage(argv[0]);
            return false;
        }

        return true;
    }

    bool build_fake_pipeline() {
        GstElement* source = gst_element_factory_make("videotestsrc", "source");
        GstElement* convert = gst_element_factory_make("videoconvert", "convert");
        GstElement* sink = create_sink();

        if (source == nullptr || convert == nullptr || sink == nullptr) {
            cleanup_partial_elements(source, nullptr, convert, sink);
            log_error("failed to create one or more GStreamer elements");
            stop();
            return false;
        }

        g_object_set(source, 
                     "is-live", TRUE,
                     nullptr);

        gst_bin_add_many(GST_BIN(pipeline_), source, convert, sink, nullptr);
        if (!gst_element_link_many(source, convert, sink, nullptr)) {
            log_error("failed to link elements");
            stop();
            return false;
        }

        source_ = source;
        convert_ = convert;
        sink_ = sink;

        if (!attach_bus()) {
            stop();
            return false;
        }

        return true;
    }

    bool build_v4l2_pipeline() {
        if (!validate_device()) {
            stop();
            return false;
        }

        GstElement* source = gst_element_factory_make("v4l2src", "source");
        GstElement* capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
        GstElement* convert = gst_element_factory_make("videoconvert", "convert");
        GstElement* sink = create_sink();
        GstElement* decoder = nullptr;

        if (options_.format == VideoFormat::kMjpeg) {
            // MJPEG needs a decoder before the frames can be displayed.
            decoder = gst_element_factory_make("jpegdec", "decoder");
        }

        if (source == nullptr || capsfilter == nullptr || convert == nullptr || sink == nullptr ||
            (options_.format == VideoFormat::kMjpeg && decoder == nullptr)) {
            cleanup_partial_elements(source, capsfilter, convert, sink, decoder);
            log_error("failed to create one or more GStreamer elements");
            stop();
            return false;
        }

        g_object_set(source, "device", options_.device.c_str(), nullptr);

        GstCaps* caps = nullptr;
        if (options_.format == VideoFormat::kMjpeg) {
            caps = gst_caps_new_simple("image/jpeg", "width", G_TYPE_INT, options_.width,
                                       "height", G_TYPE_INT, options_.height, "framerate",
                                       GST_TYPE_FRACTION, 30, 1, nullptr);
        } else {
            // YUYV is raw video, so the caps request uncompressed frames directly.
            caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "YUY2",
                                       "width", G_TYPE_INT, options_.width, "height",
                                       G_TYPE_INT, options_.height, "framerate",
                                       GST_TYPE_FRACTION, 30, 1, nullptr);
        }
        g_object_set(capsfilter, "caps", caps, nullptr);
        gst_caps_unref(caps);

        gst_bin_add(GST_BIN(pipeline_), source);
        gst_bin_add(GST_BIN(pipeline_), capsfilter);
        gst_bin_add(GST_BIN(pipeline_), convert);
        gst_bin_add(GST_BIN(pipeline_), sink);
        if (decoder != nullptr) {
            gst_bin_add(GST_BIN(pipeline_), decoder);
        }

        bool linked = false;
        if (options_.format == VideoFormat::kMjpeg) {
            linked = gst_element_link_many(source, capsfilter, decoder, convert, sink, nullptr);
        } else {
            linked = gst_element_link_many(source, capsfilter, convert, sink, nullptr);
        }

        if (!linked) {
            log_error("failed to link elements");
            stop();
            return false;
        }

        source_ = source;
        capsfilter_ = capsfilter;
        decoder_ = decoder;
        convert_ = convert;
        sink_ = sink;

        if (!attach_bus()) {
            stop();
            return false;
        }

        return true;
    }

    bool build_capture_pipeline() {
        std::cout << "capturing frame..." << std::endl;
        std::cout << "saving to file: " << options_.capture_path << std::endl;

        GstElement* source = nullptr;
        GstElement* capsfilter = nullptr;
        GstElement* convert = nullptr;
        GstElement* encoder = nullptr;
        GstElement* sink = nullptr;

        if (options_.source == SourceMode::kFake) {
            source = gst_element_factory_make("videotestsrc", "source");
            convert = gst_element_factory_make("videoconvert", "convert");
            encoder = gst_element_factory_make("jpegenc", "encoder");
            sink = gst_element_factory_make("filesink", "sink");

            if (source != nullptr) {
                g_object_set(source, 
                             "is-live", TRUE,
                             "num-buffers", 1,
                             nullptr);
            }
        } else {
            if (!validate_device()) {
                stop();
                return false;
            }

            source = gst_element_factory_make("v4l2src", "source");
            capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
            sink = gst_element_factory_make("filesink", "sink");

            if (options_.format == VideoFormat::kMjpeg) {
                g_object_set(source, "device", options_.device.c_str(), "num-buffers", 1,
                             nullptr);
            } else {
                convert = gst_element_factory_make("videoconvert", "convert");
                encoder = gst_element_factory_make("jpegenc", "encoder");
                g_object_set(source, "device", options_.device.c_str(), "num-buffers", 1,
                             nullptr);
            }
        }

        if (source == nullptr || sink == nullptr ||
            (options_.source == SourceMode::kFake && (convert == nullptr || encoder == nullptr)) ||
            (options_.source == SourceMode::kV4L2 && options_.format == VideoFormat::kMjpeg &&
             capsfilter == nullptr) ||
            (options_.source == SourceMode::kV4L2 && options_.format == VideoFormat::kYuyv &&
             (capsfilter == nullptr || convert == nullptr || encoder == nullptr))) {
            cleanup_partial_elements(source, capsfilter, convert, sink, encoder);
            log_error("failed to create one or more GStreamer elements");
            stop();
            return false;
        }

        g_object_set(sink, 
                     "location", options_.capture_path.c_str(),
                     nullptr);

        gst_bin_add(GST_BIN(pipeline_), source);
        gst_bin_add(GST_BIN(pipeline_), sink);

        bool linked = false;
        if (options_.source == SourceMode::kFake) {
            gst_bin_add(GST_BIN(pipeline_), convert);
            gst_bin_add(GST_BIN(pipeline_), encoder);
            linked = gst_element_link_many(source, 
                                           convert,
                                           encoder,
                                           sink,
                                           nullptr);
        } else if (options_.format == VideoFormat::kMjpeg) {
            GstCaps* caps = gst_caps_new_simple("image/jpeg", "width", G_TYPE_INT,
                                               options_.width, "height", G_TYPE_INT,
                                               options_.height, "framerate",
                                               GST_TYPE_FRACTION, 30, 1, nullptr);
            g_object_set(capsfilter, "caps", caps, nullptr);
            gst_caps_unref(caps);
            gst_bin_add(GST_BIN(pipeline_), capsfilter);
            linked = gst_element_link_many(source, capsfilter, sink, nullptr);
        } else {
            GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "YUY2",
                                               "width", G_TYPE_INT, options_.width, "height",
                                               G_TYPE_INT, options_.height, "framerate",
                                               GST_TYPE_FRACTION, 30, 1, nullptr);
            g_object_set(capsfilter, "caps", caps, nullptr);
            gst_caps_unref(caps);
            gst_bin_add(GST_BIN(pipeline_), capsfilter);
            gst_bin_add(GST_BIN(pipeline_), convert);
            gst_bin_add(GST_BIN(pipeline_), encoder);
            linked = gst_element_link_many(source, capsfilter, convert, encoder, sink, nullptr);
        }

        if (!linked) {
            log_error("failed to link elements");
            stop();
            return false;
        }

        source_ = source;
        capsfilter_ = capsfilter;
        convert_ = convert;
        encoder_ = encoder;
        filesink_ = sink;
        sink_ = sink;

        if (!attach_bus()) {
            stop();
            return false;
        }

        return true;
    }

    bool attach_bus() {
        bus_ = gst_element_get_bus(pipeline_);
        if (bus_ == nullptr) {
            log_error("failed to get pipeline bus");
            return false;
        }
        return true;
    }

    bool validate_device() const {
        struct stat st {
        };
        if (stat(options_.device.c_str(), &st) != 0) {
            log_error("invalid device: " + options_.device);
            return false;
        }

        if (!S_ISCHR(st.st_mode)) {
            log_error("invalid device: " + options_.device);
            return false;
        }

        return true;
    }

    GstElement* create_sink() const {
        const char* sink_name = std::getenv("GST_FAKE_CAMERA_SINK");
        if (sink_name != nullptr && sink_name[0] != '\0') {
            return gst_element_factory_make(sink_name, "sink");
        }

        return gst_element_factory_make("autovideosink", "sink");
    }

    void log_configuration() const {
        std::cout << "selected source: "
                  << (options_.source == SourceMode::kFake ? "fake" : "v4l2") << std::endl;
        std::cout << "selected device: " << options_.device << std::endl;
        std::cout << "selected format: "
                  << (options_.source == SourceMode::kFake
                          ? "fake"
                          : (options_.format == VideoFormat::kMjpeg ? "mjpeg" : "yuyv"))
                  << std::endl;
        std::cout << "requested resolution: " << options_.width << "x" << options_.height
                  << std::endl;
        if (capture_mode()) {
            std::cout << "capture file: " << options_.capture_path << std::endl;
        }
    }

    void log_negotiated_resolution() const {
        if (convert_ == nullptr) {
            return;
        }

        GstPad* pad = gst_element_get_static_pad(convert_, "src");
        if (pad == nullptr) {
            std::cout << "negotiated resolution: unavailable" << std::endl;
            return;
        }

        GstCaps* caps = gst_pad_get_current_caps(pad);
        gst_object_unref(pad);

        if (caps == nullptr) {
            std::cout << "negotiated resolution: unavailable" << std::endl;
            return;
        }

        const GstStructure* s = gst_caps_get_structure(caps, 0);
        int width = 0;
        int height = 0;
        if (gst_structure_get_int(s, "width", &width) &&
            gst_structure_get_int(s, "height", &height)) {
            std::cout << "negotiated resolution: " << width << "x" << height << std::endl;
        } else {
            std::cout << "negotiated resolution: unavailable" << std::endl;
        }

        gst_caps_unref(caps);
    }

    void handle_error(GstMessage* msg) const {
        GError* error = nullptr;
        gchar* debug_info = nullptr;
        gst_message_parse_error(msg, &error, &debug_info);

        std::cerr << "GStreamer ERROR: "
                  << (error != nullptr ? error->message : "unknown error") << std::endl;
        if (debug_info != nullptr) {
            std::cerr << "Debug info: " << debug_info << std::endl;
        }

        g_clear_error(&error);
        g_free(debug_info);
    }

    static bool parse_int(const char* text, int& value) {
        try {
            std::size_t pos = 0;
            const int parsed = std::stoi(text, &pos, 10);
            if (text[pos] != '\0') {
                return false;
            }
            value = parsed;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool require_value(int argc, int i, const char* name) const {
        if (i + 1 >= argc) {
            log_error(std::string("missing value for ") + name);
            return false;
        }
        return true;
    }

    bool capture_mode() const {
        return !options_.capture_path.empty();
    }

    void print_usage(const char* program) const {
        std::cout << "usage: " << program
                  << " [--source fake|v4l2] [--device /dev/video0] [--width N] [--height N]"
                     " [--format mjpeg|yuyv]"
                  << std::endl;
    }

    static void cleanup_partial_elements(GstElement* source, GstElement* capsfilter,
                                         GstElement* convert, GstElement* sink,
                                         GstElement* decoder = nullptr) {
        if (source != nullptr) {
            gst_object_unref(source);
        }
        if (capsfilter != nullptr) {
            gst_object_unref(capsfilter);
        }
        if (decoder != nullptr) {
            gst_object_unref(decoder);
        }
        if (convert != nullptr) {
            gst_object_unref(convert);
        }
        if (sink != nullptr) {
            gst_object_unref(sink);
        }
    }

    void log_error(const std::string& message) const {
        std::cerr << message << std::endl;
    }

    Options options_;
    GstElement* pipeline_ = nullptr;
    GstElement* source_ = nullptr;
    GstElement* capsfilter_ = nullptr;
    GstElement* decoder_ = nullptr;
    GstElement* convert_ = nullptr;
    GstElement* encoder_ = nullptr;
    GstElement* filesink_ = nullptr;
    GstElement* sink_ = nullptr;
    GstBus* bus_ = nullptr;
    bool started_ = false;
};

int main(int argc, char* argv[]) {
    FakeCameraApp app;

    if (!app.init(argc, argv)) {
        return 1;
    }

    if (!app.build_pipeline()) {
        return 1;
    }

    if (!app.start()) {
        return 1;
    }

    app.run();
    app.stop();
    return 0;
}
