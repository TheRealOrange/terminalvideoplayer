#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#include <queue>
#include <csignal>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdlib>

#include "video.h"

#ifdef HAVE_OPENCL
#include "opencl_proc.h"
#endif

// compile configuration options
// use fast perceptual diff for cpu
#define CPU_FAST_PERCEPTUAL_DIFF

// default pixel update change threshold values
#define DEFAULT_DIFFTHRESHOLD 10
#define CHANGE_THRESHOLD 10

// dithering decay value
#define CPU_DITHERING_DECAY 0.45f

// fps calculation averaging window
#define FPS_AVGING_AMT 24

// cpu floyd steinberg or atkinson dithering
// (atkinson will be slower)
#define ATKINSON_DITHERING

// use reduced character set for cpu to reduce
// computations and speedup rendering time
#define CPU_REDUCED_CHARSET_AMT 22

#include "pixelmap.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
#include <io.h>
#define write _write
#elif defined(__linux__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#endif // Windows/Linux

void get_terminal_size(int &width, int &height) {
    width = -1;
    height = -1;
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    width = (int) (csbi.srWindow.Right - csbi.srWindow.Left + 1);
    height = (int) (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
#else
    int result;
    struct winsize w{};
    //credit https://github.com/sindresorhus/macos-term-size for the macos terminal code
#if defined(__APPLE__)
    int tty_fd = open("/dev/tty", O_EVTONLY | O_NONBLOCK);
    if (tty_fd == -1) {
        fprintf(stderr, "Opening `/dev/tty` failed (%d): %s\n", errno, strerror(errno));
        return;
    }
    result = ioctl(tty_fd, TIOCGWINSZ, &w);
    close(tty_fd);
#elif defined(__linux__)
    result = ioctl(fileno(stdout), TIOCGWINSZ, &w);
#endif // MacOS/Linux
    if (result == -1) {
        fprintf(stderr, "Getting the size failed (%d): %s\n", errno, strerror(errno));
        return;
    }
    width = (int) (w.ws_col);
    height = (int) (w.ws_row);
#endif
}

const char *shapechar;
long long count = 0, curr_frame = 0;;
double fps;
int period = 0;

long long printing_time, rendering_time, decode_time, elapsed;
double avg_fps = 0;
long long total_time = 0, avg_frame_times_sum = 0;
std::queue<long long> frame_times;
int msg_y = 0;
long long dropped = 0;
double skip;

std::chrono::time_point<std::chrono::steady_clock> start, stop, render_start, render_end;
std::chrono::time_point<std::chrono::steady_clock> video_start, video_stop;
std::chrono::time_point<std::chrono::steady_clock> decode_start, decode_end;
long long total_printing_time = 0;
long long total_render_time = 0;
long long total_decode_time = 0;
int cursor_moves = 0;

// thread management for write operations
std::mutex render_buffer_mutex;
std::condition_variable buffer_ready_cv;
// double-buffering for write thread
char *render_buffer = nullptr;
int render_buffer_size = 0;
int render_buffer_written = 0;
// threading signals for next frame and shutdown
std::atomic<bool> frame_ready(false);
std::atomic<bool> write_thread_running(true);
std::thread write_thread;
// tracking print time and total printed amount in thread
std::atomic<int> last_printing_time(0);
std::atomic<long long> total_chars_printed(0ll);

// tracking usage of different unicode characters
long long char_usage[DIFF_CASES] = {0};
long long rendered_cursor_moves = 0;
long long rendered_cursor_chars = 0;

// print character usage rates
bool print_hit_rate = false;

int diff_threshold = DEFAULT_DIFFTHRESHOLD;

// char width scaling (assuming terminal chars are 2x1 hxw)
int sx = CHAR_X, sy = CHAR_X * 2;
int skipy = sy / CHAR_Y, skipx = sx / CHAR_X;

// function to intercept SIGINT such that we print the ANSI code to restore the cursor visibility
// and also print some statistics about the video played
void terminateProgram([[maybe_unused]] int sig_num) {
    write_thread_running = false;
    frame_ready = true;
    buffer_ready_cv.notify_one();
    if (write_thread.joinable()) {
        write_thread.join();
    }

    // sum total character renders
    long long total_chars = 0;
    for (const long long i: char_usage) total_chars += i;

    video_stop = std::chrono::steady_clock::now();
    const long long total_video_time = std::chrono::duration_cast<std::chrono::microseconds>(
        video_stop - video_start).count();

    // get terminal size
    int term_w, term_h;
    get_terminal_size(term_w, term_h);

    // dimensions for both boxes
    int stats_lines = 14;
    int stats_width = 45;
    int usage_width = 35;
    int spacing = 3;

    // determine if we can display side by side
    bool side_by_side = false;
    int used_chars = 0;
    int usage_lines = 0;

    if (print_hit_rate) {
        for (int i = 0; i < DIFF_CASES; i++) {
            if (char_usage[i] > 0) used_chars++;
        }
        usage_lines = used_chars + 3;
        int total_width_needed = stats_width + spacing + usage_width;
        side_by_side = (term_w >= total_width_needed + 4);
    }

    // calculate positions based on layout mode
    int stats_start_row, stats_start_col;
    int usage_start_row, usage_start_col;

    if (side_by_side) {
        // center both boxes as a combined unit
        int combined_width = stats_width + spacing + usage_width;
        int combined_height = std::max(stats_lines, usage_lines);
        stats_start_col = (term_w - combined_width) / 2;
        stats_start_row = (term_h - combined_height) / 2;
        usage_start_col = stats_start_col + stats_width + spacing;
        usage_start_row = stats_start_row;
    } else {
        // center playback stats alone
        stats_start_row = (term_h - stats_lines) / 2;
        stats_start_col = (term_w - stats_width) / 2;
        // center usage stats alone (will be displayed later)
        usage_start_row = (term_h - usage_lines) / 2;
        usage_start_col = (term_w - usage_width) / 2;
    }

    if (stats_start_row < 1) stats_start_row = 1;
    if (stats_start_col < 1) stats_start_col = 1;

    // clear and draw playback stats box
    printf("\x1B[0m");
    for (int i = 0; i < stats_lines; i++) {
        printf("\x1B[%d;%dH\x1B[48;2;0;0;0m", stats_start_row + i, stats_start_col);
        for (int j = 0; j < stats_width; j++) {
            printf(" ");
        }
    }

    // print playback statistics
    printf("\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m PLAYBACK STATISTICS ", stats_start_row + 1, stats_start_col);
    printf("\x1B[%d;%dH frames rendered:  %lld", stats_start_row + 2, stats_start_col, curr_frame);
    printf("\x1B[%d;%dH frames dropped:   %lld", stats_start_row + 3, stats_start_col, dropped);
    printf("\x1B[%d;%dH total time:       %.2fs", stats_start_row + 5, stats_start_col, (double) total_video_time / 1000000.0);
    printf("\x1B[%d;%dH decode time:      %.2fs  (%.1f%%)", stats_start_row + 6, stats_start_col,
        (double) total_decode_time / 1000000.0,
        (double) total_decode_time * 100.0 / (double) total_video_time);
    printf("\x1B[%d;%dH render time:      %.2fs  (%.1f%%)", stats_start_row + 7, stats_start_col,
        (double) total_render_time / 1000000.0,
        (double) total_render_time * 100.0 / (double) total_video_time);
    printf("\x1B[%d;%dH printing time:    %.2fs  (%.1f%%)", stats_start_row + 8, stats_start_col,
        (double) total_printing_time / 1000000.0,
        (double) total_printing_time * 100.0 / (double) total_video_time);
    printf("\x1B[%d;%dH chars rendered:   %lldk", stats_start_row + 10, stats_start_col, total_chars / 1000ll);
    printf("\x1B[%d;%dH chars printed:    %lldk", stats_start_row + 11, stats_start_col, total_chars_printed.load() / 1000ll);
    printf("\x1B[%d;%dH cursor moves:     %lldk  (%lldk chars)", stats_start_row + 12, stats_start_col,
        rendered_cursor_moves / 1000ll, rendered_cursor_chars / 1000ll);

    // move cursor to bottom of screen and show cursor
    printf("\x1B[%d;1H\u001b[?25h", term_h);

    if (print_hit_rate) {
        int sorted_indices[DIFF_CASES];
        for (int i = 0; i < DIFF_CASES; i++) sorted_indices[i] = i;

        // descending sort by usage count
        std::sort(sorted_indices, sorted_indices + DIFF_CASES,
                  [](const int a, const int b) { return char_usage[a] > char_usage[b]; });

        // determine how many entries we can display
        int max_available_lines = side_by_side ? std::max(stats_lines, term_h - 2) : term_h - 2;
        int max_entries = max_available_lines - 3; // subtract header and spacing
        if (max_entries < 1) max_entries = 1;

        int entries_to_show = std::min(used_chars, max_entries);
        int hidden_entries = used_chars - entries_to_show;
        bool show_hidden_message = (hidden_entries > 0);

        // recalculate usage_lines with truncation
        usage_lines = entries_to_show + 3; // +3 for header, spacing, and potential hidden message
        if (show_hidden_message) usage_lines++;

        // recalculate position if needed
        if (!side_by_side) {
            usage_start_row = (term_h - usage_lines) / 2;
            usage_start_col = (term_w - usage_width) / 2;
        }

        if (usage_start_row < 1) usage_start_row = 1;
        if (usage_start_col < 1) usage_start_col = 1;

        // clear and draw character usage box
        printf("\x1B[0m");
        for (int i = 0; i < usage_lines; i++) {
            printf("\x1B[%d;%dH\x1B[48;2;0;0;0m", usage_start_row + i, usage_start_col);
            for (int j = 0; j < usage_width; j++) {
                printf(" ");
            }
        }

        printf("\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m CHARACTER USAGE RATES ",
               usage_start_row + 1, usage_start_col);

        int line = 2;
        int displayed = 0;
        for (int i = 0; i < DIFF_CASES && displayed < entries_to_show; i++) {
            int idx = sorted_indices[i];
            if (char_usage[idx] > 0) {
                double percentage = (double) char_usage[idx] * 100.0 / (double) total_chars;
                printf("\x1B[%d;%dH %2d. %11lld  (%6.2f%%)  %s",
                       usage_start_row + line, usage_start_col,
                       i + 1, char_usage[idx], percentage, characters[idx]);
                line++;
                displayed++;
            }
        }

        // show hidden entries if cannot fit all entries
        if (show_hidden_message) {
            printf("\x1B[%d;%dH (... %d entries hidden)",
                   usage_start_row + line, usage_start_col, hidden_entries);
        }

        // move cursor to bottom of screen so terminal does not auto clear
        printf("\x1B[%d;1H", term_h);
    }

    fflush(stdout);
    exit(0);
}


// lookup tables
#define SRGB_TO_LINEAR_LUT_SIZE 256
#define LINEAR_TO_SRGB_LUT_SIZE 8192
#define LINEAR_TO_SRGB_SCALE (LINEAR_TO_SRGB_LUT_SIZE - 1)
#define SQRT_LUT_MAX (255*255*16)

static float srgb_to_linear_lut[SRGB_TO_LINEAR_LUT_SIZE];
static unsigned char linear_to_srgb_lut[LINEAR_TO_SRGB_LUT_SIZE];
static int sqrt_lut[SQRT_LUT_MAX];

// Original functions for LUT initialization
static float srgb_to_linear_init(const unsigned char c) {
    float v = static_cast<float>(c) / 255.0f;
    if (v <= 0.04045f)
        return v / 12.92f;
    else
        return powf((v + 0.055f) / 1.055f, 2.4f);
}

static unsigned char linear_to_srgb_init(float v) {
    if (v <= 0.0031308f)
        v = v * 12.92f;
    else
        v = 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(std::clamp(v * 255.0f, 0.0f, 255.0f));
}

// fast precomputed LUT-based conversions
inline float srgb_to_linear(const unsigned char c) {
    return srgb_to_linear_lut[c];
}

inline unsigned char linear_to_srgb(float v) {
    // clamp and quantize to LUT index
    v = std::clamp(v, 0.0f, 1.0f);
    int idx = static_cast<int>(std::lround(v * LINEAR_TO_SRGB_SCALE));
    idx = std::clamp(idx, 0, LINEAR_TO_SRGB_LUT_SIZE - 1);
    return linear_to_srgb_lut[idx];
}

void init_luts() {
    // init sqrt LUT
    for (int i = 0; i < SQRT_LUT_MAX; i++) {
        sqrt_lut[i] = static_cast<int>(sqrt(static_cast<float>(i)));
    }

    // init sRGB to linear LUT
    for (int i = 0; i < SRGB_TO_LINEAR_LUT_SIZE; i++) {
        srgb_to_linear_lut[i] = srgb_to_linear_init(static_cast<unsigned char>(i));
    }

    // init linear to sRGB LUT
    for (int i = 0; i < LINEAR_TO_SRGB_LUT_SIZE; i++) {
        float linear_value = static_cast<float>(i) / LINEAR_TO_SRGB_SCALE;
        linear_to_srgb_lut[i] = linear_to_srgb_init(linear_value);
    }
}

#ifdef CPU_FAST_PERCEPTUAL_DIFF
inline int perceptual_diff(const int r1, const int g1, const int b1, const int r2, const int g2, const int b2) {
    if (r1 > 255 || r2 > 255 || g1 > 255 || g2 > 255 || b1 > 255 || b2 > 255)
        return 9999;

    int rmean = (r1 + r2) / 2;
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;

    int idx = ((512 + rmean) * dr * dr) / 256 + 4 * dg * dg + ((767 - rmean) * db * db) / 256;
    return sqrt_lut[idx];
}
#else
inline int perceptual_diff(const int r1, const int g1, const int b1, const int r2, const int g2, const int b2) {
    if (r1 > 255 || r2 > 255 || g1 > 255 || g2 > 255 || b1 > 255 || b2 > 255)
        return 9999;

    // BT.709 coefficients for HD video
    int y1 = (r1 * 2126 + g1 * 7152 + b1 * 722) / 10000;
    int y2 = (r2 * 2126 + g2 * 7152 + b2 * 722) / 10000;
    int dy = y1 - y2;
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;

    // Weight luminance much more heavily
    int idx = 8 * dy * dy + dr * dr + dg * dg + db * db;
    return sqrt_lut[idx];
}
#endif

void write_thread_func() {
    char *write_buffer_local = nullptr;
    int write_buffer_size_local = 0;

    while (write_thread_running) {
        std::unique_lock<std::mutex> lock(render_buffer_mutex);
        buffer_ready_cv.wait(lock, [] { return frame_ready.load() || !write_thread_running.load(); });

        if (!write_thread_running && !frame_ready) break;

        if (frame_ready && render_buffer_written > 0) {
            // resize local buffer if needed
            if (!write_buffer_local || write_buffer_size_local < render_buffer_written) {
                char *temp = static_cast<char *>(std::realloc(write_buffer_local, render_buffer_written));
                if (temp) {
                    write_buffer_local = temp;
                    write_buffer_size_local = render_buffer_written;
                } else {
                    // realloc failed
                    fprintf(stderr, "failed to reallocate write buffer\n");
                    frame_ready = false;
                    lock.unlock();
                    continue;
                }
            }

            // copy render buffer to local print buffer
            memcpy(write_buffer_local, render_buffer, render_buffer_written);
            int bytes_to_write = render_buffer_written;
            frame_ready = false;

            // release lock right after copying
            lock.unlock();

            // profile write time
            std::chrono::time_point<std::chrono::steady_clock> printtime = std::chrono::steady_clock::now();
            // write entire buffer in one call
            write(STDOUT_FILENO, write_buffer_local, bytes_to_write);
            std::chrono::time_point<std::chrono::steady_clock> print_end = std::chrono::steady_clock::now();

            int printing_time_local = (int) std::chrono::duration_cast<std::chrono::microseconds>(
                print_end - printtime).count();
            last_printing_time.store(printing_time_local);

            // track the total amount we actually printed
            total_chars_printed.fetch_add(bytes_to_write);
        } else {
            lock.unlock();
        }
    }

    if (write_buffer_local) {
        std::free(write_buffer_local);
    }
}

int main(int argc, char *argv[]) {
    init_luts();
    // initialise time reference so its valid in the SIGINT handler
    video_start = std::chrono::steady_clock::now();

    // bind the function to the SIGINT signal
    signal(SIGINT, terminateProgram);

#ifdef _WIN32
    // Set console to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Enable ANSI escape sequences
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
#endif

    // Parse command line arguments
    const char *video_file = nullptr;
    bool enable_opencl = true;
    bool enable_audio = true;
    bool dither_enable = false;
    bool use_stdin = false;
    bool file_arg_provided = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [video_file|-] [diff_threshold] [options]\n", argv[0]);
            printf("If no file is provided, reads from stdin\n\n");
            printf("Options:\n");
            printf("  -                Explicitly use stdin as input\n");
            printf("  --force-cpu      Force CPU computation\n");
            printf("  --no-audio       Disable audio playback\n");
            printf("  --dither         Enable dithering\n");
            printf("  --print-usage    Print character usage rates\n");
            printf("  --help           Show this help message\n");
            return 0;
        }
        if (strcmp(argv[i], "--force-cpu") == 0) {
            enable_opencl = false;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            enable_audio = false;
        } else if (strcmp(argv[i], "--dither") == 0) {
            dither_enable = true;
        } else if (strcmp(argv[i], "--print-usage") == 0) {
            print_hit_rate = true;
        } else if (strcmp(argv[i], "-") == 0 && video_file == nullptr) {
            use_stdin = true;
            video_file = "pipe:0";  // ffmpeg name for stdin
            file_arg_provided = true;
        } else if (argv[i][0] != '-' && video_file == nullptr) {
            video_file = argv[i];
            file_arg_provided = true;
        } else if (argv[i][0] != '-' && video_file != nullptr) {
            diff_threshold = std::stoi(argv[i], nullptr, 10);
        }
    }

    // if no file provided, check if stdin is piped
    if (!file_arg_provided) {
#if defined(_WIN32)
        // Windows: check if stdin is redirected
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        bool is_piped = !GetConsoleMode(hStdin, &mode);
#else
        // Unix: check if stdin is a pipe or regular file (not a terminal)
        bool is_piped = !isatty(STDIN_FILENO);
#endif

        if (is_piped) {
            use_stdin = true;
            video_file = "pipe:0";
        } else {
            printf("\x1B[0mNo input provided. Usage:\n");
            printf("  %s <video_file>              Play a video file\n", argv[0]);
            printf("  <command> | %s               Read from pipe\n", argv[0]);
            printf("  %s --help                    Show detailed help\n", argv[0]);
            fflush(stdout);
            return 0;
        }
    }

    // skip file existence check if using stdin
    bool file_exists = use_stdin;
    if (!use_stdin) {
#if defined(__APPLE__)
        file_exists = std::__fs::filesystem::exists(video_file);
#else
        file_exists = std::filesystem::exists(video_file);
#endif
    }

    if (file_exists) {
        // if the diff threshold argument is specified, and is within range, use the specified diff
        diff_threshold = std::max(std::min(255, diff_threshold), 0);

        bool use_opencl = false;
#ifdef HAVE_OPENCL
        OpenCLProc ocl;
        if (enable_opencl) {
            use_opencl = ocl.initialize();
            if (use_opencl) {
                printf("opencl acceleration: enabled (using device: %s)\n", ocl.getDeviceName().c_str());
            } else {
                printf("opencl acceleration: disabled (no device)\n");
            }
        } else {
            printf("opencl acceleration: disabled\n");
        }
#else
        printf("opencl acceleration: disabled (not built with opencl support)\n");
#endif

        // open the video file and create the decode object
        video cap(video_file, -1, -1, enable_audio);

        // check if successfully opened
        if (!cap.isOpened()) {
            printf("\x1B[0mError opening video stream or file\n");
            fflush(stdout);
            return -1;
        }

        // get video FPS and compute period of each frame
        fps = cap.get_fps();
        period = static_cast<int>(1000000.0 / fps);

        // initialise the time reference for the frame time counter
        start = std::chrono::steady_clock::now();

        // variables used for scaling to terminal size
        int w = -1, h = -1;
        int curr_w, curr_h, orig_w = -1, orig_h = -1;
        int im_w, im_h;
        double scale_factor = 0.0;
        int small_dims[2];

        // variables used for handling the image data
        char *frame;
        char *old;
        bool alloc = false;

        // error buffer to store color errors so we can keep track and
        // diffuse color error to neighboring pixels
        float *error_buffer; // stores RGB error for next character

        // printing buffer
        char *print_buf;
        int print_buffer_size;
        int written = 0;
        int print_ret;

        // opencl buffers
        int *char_indices = nullptr;
        int *fg_colors = nullptr;
        int *bg_colors = nullptr;
        bool *needs_update = nullptr;

        // variables used for pixel update
        int diff = 0;
        int pixel[CHAR_Y][CHAR_X][3];
        bool refresh = false;
        bool begin = true;

        // variables used to see if ansi colour command needs to be reprinted
        bool bgsame = false, pixelsame = false;

        int prevpixelbg[3] = {1000, 1000, 1000};
        int pixelbg[3], pixelchar[3];
        int prevpixel[3] = {1000, 1000, 1000};

        // variables used to keep track of cursor location
        int r, c;

        // variables used to select the pixel type to print
        int mindiff, diffbg, diffpixel;
        int min_fg, min_bg, max_fg, max_bg;
        int cases[DIFF_CASES];
        int case_min = 0;

        write_thread = std::thread(write_thread_func);

        while (true) {
            count++; // count the actual number of frames printed
            curr_frame++; // count the current frame we are on

            get_terminal_size(curr_w, curr_h);

            // if the terminal size has changed, recompute scaling
            if (curr_w != orig_w || curr_h != orig_h) {
                orig_w = curr_w;
                orig_h = curr_h;
                w = curr_w;
                h = curr_h;
                h -= 1; // leave one line for the fps and other info to be printed
                msg_y = h;
                h *= sy;
                w *= sx;
                im_w = cap.get_width();
                im_h = cap.get_height();

                // get the scaling to fit the smallest dim
                scale_factor = std::min((double) w / (double) im_w, (double) h / (double) im_h);
                small_dims[0] = int((double) im_w * scale_factor);
                small_dims[1] = int((double) im_h * scale_factor);

                // if the terminal size is invalid
                if (small_dims[0] <= 0 || small_dims[1] <= 0) {
                    printf(
                        "\x1B[%d;%dHterminal dimensions is too small! (%d, %d)                                                                    \n",
                        msg_y + 1, 1, curr_w, curr_h);
                    fflush(stdout);
                    exit(0);
                }

                // force all pixels to update in the next frame
                refresh = true;

                // if this is the beginning, print some video statistics
                if (begin) {
                    begin = false;
                    printf("\x1B[?25l");
                    printf("terminal dimensions: (w %4d, h %4d)\n", curr_w, curr_h);
                    printf("frame dimensions:    (w %4d, h %4d)\n", im_w, im_h);
                    printf("display dimensions:  (w %4d, h %4d)\n", small_dims[0], small_dims[1] / (sy / sx));
                    printf("scaling:             %f\n", scale_factor);
                    printf("frames per second:   %f\n", fps);
                    if (cap.has_audio()) {
                        printf("audio:               enabled (%d Hz, %d channels)\n",
                               cap.get_audio_sample_rate(),
                               cap.get_audio_channels());
                    } else {
                        printf("audio:               not available\n");
                    }
                    fflush(stdout);

                    // wait one second so the info can be read
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

                    // set actual reference times
                    start = std::chrono::steady_clock::now();
                    video_start = std::chrono::steady_clock::now();
                }

                // set the video resize dimensions
                cap.setResize(small_dims[0], small_dims[1]);
                int video_height = cap.get_height() / sy;
                int video_width = cap.get_width() / sx;
                int term_video_chars = video_width * video_height;

                // reallocate up old frame data if they were allocated
                if (alloc) {
                    auto *realloc_frame = static_cast<char *>(std::realloc(frame, cap.get_dst_buf_size()));
                    auto *realloc_old = static_cast<char *>(std::realloc(old, cap.get_dst_buf_size()));
                    print_buffer_size = curr_w * curr_h * 60;
                    auto *realloc_print_buf = static_cast<char *>(std::realloc(print_buf, print_buffer_size));
                    auto *realloc_error = static_cast<float *>(std::realloc(
                        error_buffer, term_video_chars * 3 * sizeof(float)));

                    char *realloc_render_buf = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(render_buffer_mutex);
                        realloc_render_buf = static_cast<char *>(std::realloc(render_buffer, print_buffer_size));
                    }

#ifdef HAVE_OPENCL
                    // realloc opencl buffers
                    auto *realloc_indices = static_cast<int *>(std::realloc(
                        char_indices, term_video_chars * sizeof(int)));
                    auto *realloc_fg_colors = static_cast<int *>(
                        std::realloc(fg_colors, term_video_chars * sizeof(int)));
                    auto *realloc_bg_colors = static_cast<int *>(
                        std::realloc(bg_colors, term_video_chars * sizeof(int)));
                    auto *realloc_needs_update = static_cast<bool *>(std::realloc(
                        needs_update, term_video_chars * sizeof(bool)));

                    if (realloc_frame && realloc_old && realloc_print_buf && realloc_error
                        && realloc_indices && realloc_fg_colors && realloc_bg_colors && realloc_needs_update) {
#else
                        if (realloc_frame && realloc_old && realloc_print_buf && realloc_error) {
#endif
                        frame = realloc_frame;
                        old = realloc_old;
                        print_buf = realloc_print_buf;

                        {
                            std::lock_guard<std::mutex> lock(render_buffer_mutex);
                            render_buffer = realloc_render_buf;
                            render_buffer_size = print_buffer_size;
                        }

                        error_buffer = realloc_error;
#ifdef HAVE_OPENCL
                        char_indices = realloc_indices;
                        fg_colors = realloc_fg_colors;
                        bg_colors = realloc_bg_colors;
                        needs_update = realloc_needs_update;
#endif

                        // clear reallocated buffers
                        memset(error_buffer, 0, term_video_chars * 3 * sizeof(float));
                        memset(old, 0, cap.get_dst_buf_size());
                    } else {
                        // Free whatever succeeded before the failure
                        if (realloc_frame) std::free(realloc_frame);
                        else std::free(frame);

                        if (realloc_old) std::free(realloc_old);
                        else std::free(old);

                        if (realloc_print_buf) std::free(realloc_print_buf);
                        else std::free(print_buf);

                        if (realloc_render_buf) {
                            std::lock_guard<std::mutex> lock(render_buffer_mutex);
                            std::free(realloc_render_buf);
                        } else {
                            std::lock_guard<std::mutex> lock(render_buffer_mutex);
                            std::free(render_buffer);
                        }

                        if (realloc_error) std::free(realloc_error);
                        else std::free(error_buffer);
#ifdef HAVE_OPENCL
                        // free successfull allocated opencl buffers
                        if (realloc_indices) std::free(realloc_indices);
                        else std::free(char_indices);
                        if (realloc_fg_colors) std::free(realloc_fg_colors);
                        else std::free(fg_colors);
                        if (realloc_bg_colors) std::free(realloc_bg_colors);
                        else std::free(bg_colors);
                        if (realloc_needs_update) std::free(realloc_needs_update);
                        else std::free(needs_update);
#endif

                        fprintf(stderr, "failed to reallocate buffers on terminal resize\n");
                        alloc = false;
                        break;
                    }
                } else {
                    // otherwise create new frame buffers
                    frame = static_cast<char *>(std::malloc(cap.get_dst_buf_size()));
                    old = static_cast<char *>(std::calloc(cap.get_dst_buf_size(), sizeof(char)));
                    // allocate print buffer
                    // worst case: every single character update with color codes
                    // and every single character needs a cursor move
                    print_buffer_size = curr_w * curr_h * 60; // 60 bytes per char with safety margin
                    print_buf = static_cast<char *>(malloc(print_buffer_size));
                    // acquire lock and allocate render buffer
                    char *temp_render_buffer = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(render_buffer_mutex);
                        render_buffer_size = print_buffer_size;
                        temp_render_buffer = static_cast<char *>(malloc(render_buffer_size));
                        render_buffer = temp_render_buffer;
                    }

                    // allocate dithering error buffer
                    error_buffer = static_cast<float *>(std::calloc(term_video_chars * 3, sizeof(float)));

#ifdef HAVE_OPENCL
                    // Allocate OpenCL buffers
                    char_indices = static_cast<int *>(std::malloc(term_video_chars * sizeof(int)));
                    fg_colors = static_cast<int *>(std::malloc(term_video_chars * sizeof(int)));
                    bg_colors = static_cast<int *>(std::malloc(term_video_chars * sizeof(int)));
                    needs_update = static_cast<bool *>(std::malloc(term_video_chars * sizeof(bool)));

                    if (frame && old && print_buf && error_buffer
                        && char_indices && fg_colors && bg_colors && needs_update) {
#else
                        if (frame && old && print_buf && error_buffer) {
#endif
                        alloc = true;
                    } else {
                        // clean up partial allocations
                        if (frame) std::free(frame);
                        if (old) std::free(old);
                        if (print_buf) std::free(print_buf);
                        if (temp_render_buffer) {
                            std::lock_guard<std::mutex> lock(render_buffer_mutex);
                            std::free(render_buffer);
                            render_buffer = nullptr;
                        }
                        if (error_buffer) std::free(error_buffer);

#ifdef HAVE_OPENCL
                        // cleanup partial opencl allocations
                        if (char_indices) std::free(char_indices);
                        if (fg_colors) std::free(fg_colors);
                        if (bg_colors) std::free(bg_colors);
                        if (needs_update) std::free(needs_update);
#endif

                        alloc = false;
                        fprintf(stderr, "failed to allocate buffers\n");
                        break;
                    }
                }

                // set the entire screen to black
                written = snprintf(print_buf, print_buffer_size, "\x1B[2J\x1B[H\x1B[48;2;0;0;0m");

                // one write call for frame
                write(STDOUT_FILENO, print_buf, written);
            }

            if (!alloc) break;

            // get frame from video
            decode_start = std::chrono::steady_clock::now();
            int ret = cap.get_frame(small_dims[0], small_dims[1], frame);
            decode_end = std::chrono::steady_clock::now();
            decode_time = (int) std::chrono::duration_cast<std::chrono::microseconds>(decode_end - decode_start).count();
            total_decode_time += decode_time;

            // decay error buffer to prevent temporal ghosting
            int video_height = cap.get_height() / sy;
            int video_width = cap.get_width() / sx;
            if (!use_opencl) {
                if (error_buffer && dither_enable) {
                    for (int i = 0; i < video_height * video_width * 3; i++) {
                        error_buffer[i] *= CPU_DITHERING_DECAY;
                    }
                }
            }

            // compute time taken for the previous frame
            stop = std::chrono::steady_clock::now();
            elapsed = std::chrono::duration_cast<std::chrono::microseconds>(stop - video_start).count();
            long long frame_time = std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
            start = std::chrono::steady_clock::now();

            // compute the average fps, as well as the fps of the last N frames
            total_time = elapsed;
            avg_fps = static_cast<double>(count) * 1000000.0 / static_cast<double>(total_time);
            frame_times.push(frame_time);
            avg_frame_times_sum += frame_time;
            if (frame_times.size() > FPS_AVGING_AMT) {
                avg_frame_times_sum -= frame_times.front();
                frame_times.pop();
            }

            // if there is still time before the next frame, wait a bit
            if (curr_frame * period - elapsed > 0)
                std::this_thread::sleep_until(
                    std::chrono::microseconds(curr_frame * period - elapsed - avg_frame_times_sum / frame_times.size())
                    +
                    stop);
            else {
                // if the next frame is overdue, skip the frame and wait till the earliest non-overdue frame
                skip = static_cast<double>(elapsed) / static_cast<double>(period) - static_cast<double>(curr_frame);
                for (int i = 0; i < std::floor(skip); i++) ret = cap.get_frame(small_dims[0], small_dims[1], frame);
                dropped += std::floor(skip);
                curr_frame += std::floor(skip);
                std::this_thread::sleep_until(
                    std::chrono::microseconds(curr_frame * period - avg_frame_times_sum / frame_times.size()) +
                    video_start);
            }

            // set the previous pixel bg colour and font colour to a large value to force the ansi colour command to be printed
            // for the first pixel in each frame
            prevpixelbg[0] = 1000;
            prevpixelbg[1] = 1000;
            prevpixelbg[2] = 1000;
            prevpixel[0] = 1000;
            prevpixel[1] = 1000;
            prevpixel[2] = 1000;

            // if the video is over, break
            if (cap.is_end_of_stream()) break;
            // if the frame is empty, break immediately
            if (ret < 0) {
                printf("\x1B[0mError reading video stream or file\n");
                break;
            }

            // force the first pixel to use the ansi cursor move command
            r = -1;
            c = -1;

            // reset snprintf buffer
            written = 0;
            // reset cursor moves tracking
            cursor_moves = 0;
            // start tracking render time
            render_start = std::chrono::steady_clock::now();

#ifdef HAVE_OPENCL
            if (use_opencl) {
                ocl.processFrame(
                    frame, old, old,
                    cap.get_width(), cap.get_height(),
                    video_width, video_height,
                    diff_threshold, refresh, dither_enable,
                    char_indices, fg_colors, bg_colors, needs_update
                );

                // generate print output from opencl results
                for (int ay = 0; ay < video_height; ay++) {
                    for (int x = 0; x < video_width; x++) {
                        int char_idx = ay * video_width + x;

                        if (needs_update[char_idx]) {
                            // unpack colors
                            pixelchar[2] = (fg_colors[char_idx] >> 16) & 0xFF;
                            pixelchar[1] = (fg_colors[char_idx] >> 8) & 0xFF;
                            pixelchar[0] = fg_colors[char_idx] & 0xFF;

                            pixelbg[2] = (bg_colors[char_idx] >> 16) & 0xFF;
                            pixelbg[1] = (bg_colors[char_idx] >> 8) & 0xFF;
                            pixelbg[0] = bg_colors[char_idx] & 0xFF;

                            char_usage[char_indices[char_idx]]++;
                            shapechar = characters[char_indices[char_idx]];

                            bgsame = false;
                            pixelsame = false;
                            diffbg = perceptual_diff(
                                prevpixelbg[2], prevpixelbg[1], prevpixelbg[0],
                                pixelbg[2], pixelbg[1], pixelbg[0]
                            );
                            diffpixel = perceptual_diff(
                                prevpixel[2], prevpixel[1], prevpixel[0],
                                pixelchar[2], pixelchar[1], pixelchar[0]
                            );

                            if (diffbg < CHANGE_THRESHOLD) {
                                for (int k = 0; k < 3; k++) pixelbg[k] = prevpixelbg[k];
                                bgsame = true;
                            } else {
                                for (int k = 0; k < 3; k++) prevpixelbg[k] = pixelbg[k];
                            }

                            if (diffpixel < CHANGE_THRESHOLD) {
                                for (int k = 0; k < 3; k++) pixelchar[k] = prevpixel[k];
                                pixelsame = true;
                            } else {
                                for (int k = 0; k < 3; k++) prevpixel[k] = pixelchar[k];
                            }

                            if (r != ay || c != x) {
                                cursor_moves++;
                                if (written >= print_buffer_size - 1) {
                                    fprintf(stderr, "print buffer full at %d bytes\n", written);
                                    break;
                                }
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[%d;%dH", ay + 1, x + 1);
                                if (print_ret > 0 && print_ret < print_buffer_size - written) {
                                    written += print_ret;
                                    rendered_cursor_moves++;
                                    rendered_cursor_chars += print_ret;
                                }
                            }

                            if (written >= print_buffer_size - 1) {
                                fprintf(stderr, "print buffer full at %d bytes\n", written);
                                break;
                            }
                            if (!bgsame && !pixelsame)
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[48;2;%d;%d;%d;38;2;%d;%d;%dm%s",
                                                     pixelbg[2], pixelbg[1], pixelbg[0],
                                                     pixelchar[2], pixelchar[1], pixelchar[0], shapechar);
                            else if (!bgsame)
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[48;2;%d;%d;%dm%s",
                                                     pixelbg[2], pixelbg[1], pixelbg[0], shapechar);
                            else if (!pixelsame)
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[38;2;%d;%d;%dm%s",
                                                     pixelchar[2], pixelchar[1], pixelchar[0], shapechar);
                            else
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "%s", shapechar);

                            if (print_ret > 0 && print_ret < print_buffer_size - written)
                                written += print_ret;

                            r = ay;
                            c = x + 1;
                            if (c == curr_w) {
                                c = 0;
                                r++;
                            }
                        }
                    }
                }
            } else {
                // variables to store the pointer to the start of each row for easier reference
                // each pixel uses CHAR_Y rows of the actual image
                char *row[CHAR_Y];
                char *oldrow[CHAR_Y];
#endif
                for (int ay = 0; ay < video_height; ay++) {
                    // set the row pointers
                    for (int i = 0; i < CHAR_Y; i++) {
                        row[i] = frame + (ay * sy + i * skipy) * 3 * cap.get_width();
                        oldrow[i] = old + (ay * sy + i * skipy) * 3 * cap.get_width();
                    }
                    for (int x = 0; x < video_width; x++) {
                        // get the colour values of the pixels of the current character
                        for (int i = 0; i < CHAR_Y; i++)
                            for (int j = 0; j < CHAR_X; j++)
                                for (int k = 0; k < 3; k++) {
                                    pixel[i][j][k] = static_cast<unsigned char>(*(
                                        row[i] + (x * sx + j * skipx) * 3 + k));

                                    if (dither_enable) {
                                        // apply error from previous character
                                        int err_idx = (ay * video_width + x) * 3 + k;
                                        pixel[i][j][k] = std::clamp(
                                            pixel[i][j][k] + static_cast<int>(error_buffer[err_idx]), 0, 255);
                                    }
                                }

                        diff = 0;
                        // if a refresh is necessary, set the diff to the max diff
                        if (refresh) {
                            diff = 255;
                        } else {
                            // otherwise, otherwise, calculate the perceptual weighted color differences
                            // in the RGB values between the actual video frame and what is on screen
                            // for each pixel that makes up the character
                            for (int i = 0; i < CHAR_Y; i++)
                                for (int j = 0; j < CHAR_X; j++) {
                                    int old_b = static_cast<unsigned char>(*(oldrow[i] + (x * sx + j * skipx) * 3 + 0));
                                    int old_g = static_cast<unsigned char>(*(oldrow[i] + (x * sx + j * skipx) * 3 + 1));
                                    int old_r = static_cast<unsigned char>(*(oldrow[i] + (x * sx + j * skipx) * 3 + 2));

                                    diff = std::max(diff, perceptual_diff(
                                                        old_r, old_g, old_b,
                                                        pixel[i][j][2], pixel[i][j][1], pixel[i][j][0]
                                                    ));
                                }
                        }

                        // if the difference exceeds the set threshold, reprint the entire character
                        if (diff >= diff_threshold) {
                            for (int &case_it: cases) case_it = 0;

                            // calculate for each unicode character, the max error between what
                            // will be printed on screen and the actual video pixel if the character were used
                            // for the cpu version, just use max, the opencl version can use MSE
                            for (int k = 0; k < 3; k++) {
                                for (int case_it = 0; case_it < DIFF_CASES - CPU_REDUCED_CHARSET_AMT; case_it++) {
                                    min_fg = 256;
                                    min_bg = 256;
                                    max_fg = 0;
                                    max_bg = 0;
                                    // for every character, there is a foreground colour and background colour
                                    // so we just check for the max and the min of all the values for pixels which
                                    // belong to the foreground and background regions respectively
                                    // the diff between the max and the min is the max error
                                    for (int i = 0; i < CHAR_Y; i++)
                                        for (int j = 0; j < CHAR_X; j++) {
                                            if (pixelmap[case_it][i * CHAR_X + j]) {
                                                min_fg = std::min(min_fg, pixel[i][j][k]);
                                                max_fg = std::max(max_fg, pixel[i][j][k]);
                                            } else {
                                                min_bg = std::min(min_bg, pixel[i][j][k]);
                                                max_bg = std::max(max_bg, pixel[i][j][k]);
                                            }
                                        }
                                    cases[case_it] = std::max(cases[case_it],
                                                              std::max(max_fg - min_fg, max_bg - min_bg));
                                }
                            }

                            // choose the unicode char to print which minimises the diff
                            mindiff = 256;
                            case_min = 0;
                            for (int case_it = 0; case_it < DIFF_CASES - CPU_REDUCED_CHARSET_AMT; case_it++) {
                                if (cases[case_it] < mindiff) {
                                    case_min = case_it;
                                    mindiff = cases[case_it];
                                }
                            }

                            char_usage[case_min]++;
                            shapechar = characters[case_min];

                            diffbg = 0;
                            diffpixel = 0;
                            bgsame = false;
                            pixelsame = false;

                            // based on the unicode character selected, find the avg colour of the pixels
                            // in the foreground region and background region
                            // the avg colour will be used as the colour to be printed
                            float linear_fg[3] = {0, 0, 0};
                            float linear_bg[3] = {0, 0, 0};
                            int bg_count = 0, fg_count = 0;

                            for (int i = 0; i < CHAR_Y; i++)
                                for (int j = 0; j < CHAR_X; j++) {
                                    if (pixelmap[case_min][i * CHAR_X + j]) {
                                        for (int k = 0; k < 3; k++)
                                            linear_fg[k] += srgb_to_linear(pixel[i][j][k]);
                                        fg_count++;
                                    } else {
                                        for (int k = 0; k < 3; k++)
                                            linear_bg[k] += srgb_to_linear(pixel[i][j][k]);
                                        bg_count++;
                                    }
                                }

                            for (int k = 0; k < 3; k++) {
                                pixelchar[k] = linear_to_srgb(linear_fg[k] / static_cast<float>(fg_count));
                                pixelbg[k] = linear_to_srgb(linear_bg[k] / static_cast<float>(bg_count));
                            }

                            // find the max diff between the foreground and background colours
                            // of the previously printed character
                            diffbg = perceptual_diff(
                                prevpixelbg[2], prevpixelbg[1], prevpixelbg[0],
                                pixelbg[2], pixelbg[1], pixelbg[0]
                            );
                            diffpixel = perceptual_diff(
                                prevpixel[2], prevpixel[1], prevpixel[0],
                                pixelchar[2], pixelchar[1], pixelchar[0]
                            );

                            // if the foreground or background colours are sufficiently similar,
                            // we don't need to print the ansi command again

                            // but if we skip printing the ansi command to change colour,
                            // we have to remember to keep track of the actual colour of this character
                            // on screen, which will be the colour of previous one
                            if (diffbg < CHANGE_THRESHOLD) {
                                for (int k = 0; k < 3; k++) pixelbg[k] = prevpixelbg[k];
                                bgsame = true;
                            } else
                                for (int k = 0; k < 3; k++) prevpixelbg[k] = pixelbg[k];
                            if (diffpixel < CHANGE_THRESHOLD) {
                                for (int k = 0; k < 3; k++) pixelchar[k] = prevpixel[k];
                                pixelsame = true;
                            } else
                                for (int k = 0; k < 3; k++) prevpixel[k] = pixelchar[k];

                            // store the actual colour of the character's pixels in a buffer to check diff next time
                            for (int k = 0; k < 3; k++)
                                for (int i = 0; i < CHAR_Y; i++)
                                    for (int j = 0; j < CHAR_X; j++) {
                                        if (pixelmap[case_min][i * CHAR_X + j])
                                            *(oldrow[i] + (x * sx + j * skipx) * 3 + k) = static_cast<char>(pixelchar[
                                                k]);
                                        else
                                            *(oldrow[i] + (x * sx + j * skipx) * 3 + k) = static_cast<char>(pixelbg[k]);
                                    }

                            if (dither_enable) {
                                // diffuse color errors
                                for (int k = 0; k < 3; k++) {
                                    float total_error = 0;
                                    for (int i = 0; i < CHAR_Y; i++) {
                                        for (int j = 0; j < CHAR_X; j++) {
                                            int target = pixelmap[case_min][i * CHAR_X + j] ? pixelchar[k] : pixelbg[k];
                                            total_error += static_cast<float>(pixel[i][j][k] - target);
                                        }
                                    }
                                    total_error /= (CHAR_Y * CHAR_X);

                                    // distribute error per channel
                                    int err_idx_right = (ay * video_width + (x + 1)) * 3 + k;
                                    int err_idx_below = ((ay + 1) * video_width + x) * 3 + k;
                                    int err_idx_diag = ((ay + 1) * video_width + (x + 1)) * 3 + k;

#ifdef ATKINSON_DITHERING
                                    // atkinson dithering
                                    if (x + 1 < video_width)
                                        error_buffer[err_idx_right] += total_error * 0.125f;
                                    if (x + 2 < video_width)
                                        error_buffer[(ay * video_width + (x + 2)) * 3 + k] += total_error * 0.125f;
                                    if (ay + 1 < video_height) {
                                        error_buffer[err_idx_below] += total_error * 0.125f;
                                        if (x - 1 >= 0)
                                            error_buffer[((ay + 1) * video_width + (x - 1)) * 3 + k] += total_error *
                                                    0.125f;
                                        if (x + 1 < video_width)
                                            error_buffer[err_idx_diag] += total_error * 0.125f;
                                    }
                                    if (ay + 2 < video_height)
                                        error_buffer[((ay + 2) * video_width + x) * 3 + k] += total_error * 0.125f;
#else
                                    // floyd-steinberg dithering
                                    if (x + 1 < video_width)
                                        error_buffer[err_idx_right] += total_error * 0.4375f; // 7/16 right
                                    if (ay + 1 < video_height)
                                        error_buffer[err_idx_below] += total_error * 0.3125f; // 5/16 below
                                    if (x + 1 < video_width && ay + 1 < video_height)
                                        error_buffer[err_idx_diag] += total_error * 0.25f; // 4/16 diagonal
#endif
                                }
                            }

                            // if the cursor is already in the right position, do not print the ansi move cursor command
                            // the ansi position command is one indexed
                            if (r != ay || c != x) {
                                cursor_moves++;
                                if (written >= print_buffer_size - 1) {
                                    fprintf(stderr, "print buffer full at %d bytes\n", written);
                                    break;
                                }
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[%d;%dH", ay + 1, x + 1);
                                if (print_ret > 0 && print_ret < print_buffer_size - written) {
                                    written += print_ret;
                                    rendered_cursor_moves++;
                                    rendered_cursor_chars += print_ret;
                                }
                            }

                            // prints background and foreground colour change command, or either of them, or none
                            // depending on the previously computed difference
                            // color codes and character
                            if (written >= print_buffer_size - 1) {
                                fprintf(stderr, "print buffer full at %d bytes\n", written);
                                break;
                            }
                            if (!bgsame && !pixelsame)
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[48;2;%d;%d;%d;38;2;%d;%d;%dm%s",
                                                     pixelbg[2], pixelbg[1], pixelbg[0],
                                                     pixelchar[2], pixelchar[1], pixelchar[0], shapechar);
                            else if (!bgsame)
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[48;2;%d;%d;%dm%s",
                                                     pixelbg[2], pixelbg[1], pixelbg[0], shapechar);
                            else if (!pixelsame)
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "\x1B[38;2;%d;%d;%dm%s",
                                                     pixelchar[2], pixelchar[1], pixelchar[0], shapechar);
                            else
                                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                                     "%s", shapechar);

                            if (print_ret > 0 && print_ret < print_buffer_size - written)
                                written += print_ret;

                            // advance the cursor to keep track of where it is
                            r = ay;
                            c = x + 1;
                            if (c == curr_w) {
                                c = 0;
                                r++;
                            }
                        }
                    }
                }

#ifdef HAVE_OPENCL
            }
#endif
            refresh = false;
            render_end = std::chrono::steady_clock::now();
            rendering_time = (int) std::chrono::duration_cast<std::chrono::microseconds>(render_end - render_start).
                    count();
            total_render_time += rendering_time;
            // print the fps, avg fps, dropped frames, etc. at the bottom of the video
            if (written >= print_buffer_size - 1) {
                fprintf(stderr, "print buffer full at %d bytes\n", written);
                break;
            }
            // different formatting based on terminal width
            if (curr_w >= 172) {
                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                     "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m  fps: %6.2f  |  avg: %6.2f  |  decode: %6.1fms  |  render: %6.1fms  |  print: %6.1fms  |  cursor: %5d  |  chars: %6.1fk  |  dropped: %7lld  |  frame: %7lld   ",
                                     msg_y + 1, 1,
                                     static_cast<double>(frame_times.size()) * 1000000.0 / static_cast<double>(avg_frame_times_sum),
                                     avg_fps,
                                     static_cast<double>(decode_time) / 1000.0,
                                     static_cast<double>(rendering_time) / 1000.0,
                                     static_cast<double>(printing_time) / 1000.0,
                                     cursor_moves, written / 1000.0, dropped, curr_frame);
            } else if (curr_w >= 125) {
                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                     "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m  fps: %6.2f  |  decode: %5.1fms  |  render: %5.1fms  |  print: %5.1fms  |  dropped: %7lld  |  frame: %7lld   ",
                                     msg_y + 1, 1,
                                     static_cast<double>(frame_times.size()) * 1000000.0 / static_cast<double>(avg_frame_times_sum),
                                     static_cast<double>(decode_time) / 1000.0,
                                     static_cast<double>(rendering_time) / 1000.0,
                                     static_cast<double>(printing_time) / 1000.0,
                                     dropped, curr_frame);
            } else if (curr_w >= 88) {
                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                     "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m  fps: %6.2f  |  d: %5.1f  r: %5.1f  p: %5.1f  |  frame: %7lld  drop: %5lld   ",
                                     msg_y + 1, 1,
                                     static_cast<double>(frame_times.size()) * 1000000.0 / static_cast<double>(avg_frame_times_sum),
                                     static_cast<double>(decode_time) / 1000.0,
                                     static_cast<double>(rendering_time) / 1000.0,
                                     static_cast<double>(printing_time) / 1000.0,
                                     curr_frame, dropped);
            } else if (curr_w >= 56) {
                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                     "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m  fps: %5.1f  |  frame: %7lld  |  dropped: %5lld   ",
                                     msg_y + 1, 1,
                                     static_cast<double>(frame_times.size()) * 1000000.0 / static_cast<double>(avg_frame_times_sum),
                                     curr_frame, dropped);
            } else if (curr_w >= 40) {
                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                     "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m  fps: %5.1f  |  f: %7lld  d: %5lld ",
                                     msg_y + 1, 1,
                                     static_cast<double>(frame_times.size()) * 1000000.0 / static_cast<double>(avg_frame_times_sum),
                                     curr_frame, dropped);
            } else {
                print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                     "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m  %5.1ffps  f:%lld ",
                                     msg_y + 1, 1,
                                     static_cast<double>(frame_times.size()) * 1000000.0 / static_cast<double>(avg_frame_times_sum),
                                     curr_frame);
            }
            if (print_ret > 0 && print_ret < print_buffer_size - written)
                written += print_ret;

            // send buffer to printing thread
            {
                std::lock_guard<std::mutex> lock(render_buffer_mutex);
                memcpy(render_buffer, print_buf, written);
                render_buffer_written = written;
                frame_ready = true;
            }
            buffer_ready_cv.notify_one();

            // get last printing time from write thread
            printing_time = last_printing_time.load();
            total_printing_time += printing_time;
        }

        // free the buffers when the video completes
        if (alloc) {
            std::free(frame);
            std::free(old);
            std::free(print_buf);
            std::free(error_buffer);
        }
    } else {
        printf("\x1B[0mfile not found\n");
        fflush(stdout);
        exit(0);
    }
    terminateProgram(0);
    return 0;
}
