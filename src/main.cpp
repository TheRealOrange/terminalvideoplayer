#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#include <queue>
#include <csignal>
#include <cstdlib>

#include "video.h"

#define CHANGE_THRESHOLD 3

#define CHAR_Y 4
#define CHAR_X 4
#define DIFF_CASES 11

const char characters[DIFF_CASES][4] = {"\u2584", // bottom half block
                                        "\u2590", // right half block
                                        "\u2598", // top left quarter
                                        "\u259d", // top right quarter
                                        "\u2596", // bottom left quarter
                                        "\u2597", // bottom right quarter
                                        "\u259e", // diagonal
                                        "\u2582", // lower quarter block
                                        "\u2586", // lower 3 quarters block
                                        "\u258e",
                                        "\u258a"};

const int pixelmap[DIFF_CASES][CHAR_Y * CHAR_X] = {{0, 0, 0, 0,
                                                    0, 0, 0, 0,
                                                    1, 1, 1, 1,
                                                    1, 1, 1, 1},
                                                   {0, 0, 1, 1,
                                                    0, 0, 1, 1,
                                                    0, 0, 1, 1,
                                                    0, 0, 1, 1},
                                                   {1, 1, 0, 0,
                                                    1, 1, 0, 0,
                                                    0, 0, 0, 0,
                                                    0, 0, 0, 0},
                                                   {0, 0, 1, 1,
                                                    0, 0, 1, 1,
                                                    0, 0, 0, 0,
                                                    0, 0, 0, 0},
                                                   {0, 0, 0, 0,
                                                    0, 0, 0, 0,
                                                    1, 1, 0, 0,
                                                    1, 1, 0, 0},
                                                   {0, 0, 0, 0,
                                                    0, 0, 0, 0,
                                                    0, 0, 1, 1,
                                                    0, 0, 1, 1},
                                                   {0, 0, 1, 1,
                                                    0, 0, 1, 1,
                                                    1, 1, 0, 0,
                                                    1, 1, 0, 0},
                                                   {0, 0, 0, 0,
                                                    0, 0, 0, 0,
                                                    0, 0, 0, 0,
                                                    1, 1, 1, 1},
                                                   {0, 0, 0, 0,
                                                    1, 1, 1, 1,
                                                    1, 1, 1, 1,
                                                    1, 1, 1, 1},
                                                   {1, 0, 0, 0,
                                                    1, 0, 0, 0,
                                                    1, 0, 0, 0,
                                                    1, 0, 0, 0},
                                                   {1, 1, 1, 0,
                                                    1, 1, 1, 0,
                                                    1, 1, 1, 0,
                                                    1, 1, 1, 0}};

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
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
    width = (int)(csbi.srWindow.Right-csbi.srWindow.Left+1);
    height = (int)(csbi.srWindow.Bottom-csbi.srWindow.Top+1);
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
int count = 0, curr_frame = 0;;
double fps;
int period = 0;

int printing_time, elapsed;
double avg_fps = 0;
int total_time = 0, frame10_time = 0;
std::queue<int> frametimes;
int msg_y = 0;
int dropped = 0;
double skip;

std::chrono::time_point<std::chrono::steady_clock> start, stop, videostart, videostop, printtime;
long long total_printing_time = 0;

int diffthreshold = 10;

int sx = CHAR_X, sy = CHAR_X*2;
int skipy = sy / CHAR_Y, skipx = sx / CHAR_X;

// function to intercept SIGINT such that we print the ANSI code to restore the cursor visibility
// and also print some statistics about the video played
void terminateProgram([[maybe_unused]] int sig_num) {
    videostop = std::chrono::steady_clock::now();
    long long total_video_time = (long long) std::chrono::duration_cast<std::chrono::microseconds>(
            videostop - videostart).count();
    printf("\x1B[0m\x1B[%d;%dHframes: %5d, dropped: %5d,  total time: %5.2fs,  printing time: %5.2fs                                                            \u001b[?25h",
           msg_y+1, 1, curr_frame, dropped, (double) total_video_time / 1000000.0,
           (double) total_printing_time / 1000000.0);
    fflush(stdout);
    exit(0);
}

int main(int argc, char *argv[]) {
    // initialise time reference so its valid in the SIGINT handler
    videostart = std::chrono::steady_clock::now();

    // bind the function to the SIGINT signal
    signal(SIGINT, terminateProgram);

    // check if number of arguments is correct
    if (argc <= 1 || strlen(argv[1]) <= 0) {
        printf("\x1B[0mplease provide the filename as the first input argument");
        fflush(stdout);
        return 0;
    }
// apparently macos uses std::__fs::filesystem
#if defined(__APPLE__)
    if (std::__fs::filesystem::exists(argv[1])) {
#else
    if (std::filesystem::exists(argv[1])) {
#endif
        // if the diff threshold argument is specified, and is within range, use the specified diff
        if (argc > 2) diffthreshold = std::stoi(argv[2], nullptr, 10);
        diffthreshold = std::max(std::min(255, diffthreshold), 0);

        // open the video file and create the decode object
        video cap(argv[1]);

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

        // printing buffer
        char *print_buf;
        int print_buffer_size;
        int written = 0;
        int print_ret;

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

        while (true) {
            count++;      // count the actual number of frames printed
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
                    printf("\x1B[%d;%dHterminal dimensions is too small! (%d, %d)                                                                    \n",
                           msg_y+1, 1, curr_w, curr_h);
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
                    fflush(stdout);

                    // wait one second so the info can be read
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

                    // set actual reference times
                    start = std::chrono::steady_clock::now();
                    videostart = std::chrono::steady_clock::now();
                }

                // set the video resize dimensions
                cap.setResize(small_dims[0], small_dims[1]);

                // reallocate up old frame data if they were allocated
                if (alloc) {
                    char* realloc_frame = static_cast<char *>(std::realloc(frame, cap.get_dst_buf_size()));
                    char* realloc_old = static_cast<char *>(std::realloc(old, cap.get_dst_buf_size()));
                    print_buffer_size = curr_w * curr_h * 60;
                    char* realloc_print_buf = static_cast<char *>(std::realloc(print_buf, print_buffer_size));

                    if (realloc_frame && realloc_old && realloc_print_buf) {
                        frame = realloc_frame;
                        old = realloc_old;
                        print_buf = realloc_print_buf;
                        memset(old, 0, cap.get_dst_buf_size());
                    } else {
                        // Free whatever succeeded before the failure
                        if (realloc_frame) std::free(realloc_frame);
                        else std::free(frame);

                        if (realloc_old) std::free(realloc_old);
                        else std::free(old);

                        if (realloc_print_buf) std::free(realloc_print_buf);
                        else std::free(print_buf);

                        fprintf(stderr, "Failed to reallocate buffers for terminal resize\n");
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
                    print_buffer_size = curr_w * curr_h * 60;  // 60 bytes per char with safety margin
                    print_buf = static_cast<char *>(malloc(print_buffer_size));

                    if (frame && old && print_buf) {
                        alloc = true;
                    } else {
                        // clean up partial allocations
                        if (frame) std::free(frame);
                        if (old) std::free(old);
                        if (print_buf) std::free(print_buf);
                        alloc = false;
                        break;
                    }
                }

                // set the entire screen to black
                written = snprintf(print_buf, print_buffer_size,"\x1B[0;0H\x1B[48;2;0;0;0m");
                if (written > 0 && written < print_buffer_size) {
                    for (int i = 0; i < curr_w * curr_h; i++) {
                        print_ret = snprintf(print_buf + written, print_buffer_size - written, " ");
                        if (print_ret > 0 && print_ret < print_buffer_size - written) written += print_ret;
                        else break;
                    }
                }

                // one write call for frame
                write(STDOUT_FILENO, print_buf, written);
            }

            // get frame from video
            int ret = cap.get_frame(small_dims[0], small_dims[1], frame);

            // compute time taken for the previous frame
            stop = std::chrono::steady_clock::now();
            elapsed = (int) std::chrono::duration_cast<std::chrono::microseconds>(stop - videostart).count();
            int frame_time = (int) std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
            start = std::chrono::steady_clock::now();

            // compute the average fps, as well as the fps of the last 10 frames
            total_time = elapsed;
            avg_fps = (double) count * 1000000.0 / (double) total_time;
            frametimes.push(frame_time);
            frame10_time += frame_time;
            if (frametimes.size() > 10) {
                frame10_time -= frametimes.front();
                frametimes.pop();
            }

            // if there is still time before the next frame, wait a bit
            if (curr_frame * period - elapsed > 0)
                std::this_thread::sleep_until(
                        std::chrono::microseconds(curr_frame * period - elapsed - frame10_time / frametimes.size()) +
                        stop);
            else {
                // if the next frame is overdue, skip the frame and wait till the earliest non-overdue frame
                skip = static_cast<double>(elapsed) / static_cast<double>(period) - static_cast<double>(curr_frame);
                for (int i = 0; i < std::floor(skip); i++) ret = cap.get_frame(small_dims[0], small_dims[1], frame);
                dropped += std::floor(skip);
                curr_frame += std::floor(skip);
                std::this_thread::sleep_until(
                        std::chrono::microseconds(curr_frame * period - frame10_time / frametimes.size()) + videostart);
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

            // variables to store the pointer to the start of each row for easier reference
            // each pixel uses CHAR_Y rows of the actual image
            char *row[CHAR_Y];
            char *oldrow[CHAR_Y];

            // reset snprintf buffer
            written = 0;

            for (int ay = 0; ay < cap.get_height() / sy; ay++) {
                // set the row pointers
                for (int i = 0; i < CHAR_Y; i++) {
                    row[i] = frame + (ay * sy + i * skipy) * 3 * cap.get_width();
                    oldrow[i] = old + (ay * sy + i * skipy) * 3 * cap.get_width();
                }
                for (int x = 0; x < cap.get_width() / sx; x++) {
                    // get the colour values of the pixels of the current character
                    for (int i = 0; i < CHAR_Y; i++)
                        for (int j = 0; j < CHAR_X; j++)
                            for (int k = 0; k < 3; k++)
                                pixel[i][j][k] = (unsigned char) (*(row[i] + (x * sx + j * skipx) * 3 + k));

                    diff = 0;
                    // if a refresh is necessary, set the diff to the max diff
                    if (refresh) {
                        diff = 255;
                    } else {
                        // otherwise, find the max difference in RGB values between the actual
                        // video frame and what is on screen for each pixel that makes up the character
                        for (int i = 0; i < CHAR_Y; i++)
                            for (int j = 0; j < CHAR_X; j++)
                                for (int k = 0; k < 3; k++)
                                    diff = std::max(diff, std::abs(
                                            static_cast<unsigned char>(*(oldrow[i] + (x * sx + j * skipx) * 3 + k)) -
                                            pixel[i][j][k]));
                    }

                    // if the difference exceeds the set threshold, reprint the entire character
                    if (diff >= diffthreshold) {
                        for (int &case_it: cases) case_it = 0;

                        // calculate for each unicode character, the max error between what
                        // will be printed on screen and the actual video pixel if the character were used
                        for (int k = 0; k < 3; k++) {
                            for (int case_it = 0; case_it < (int)(sizeof(cases) / sizeof(cases[0])); case_it++) {
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
                                cases[case_it] = std::max(cases[case_it], std::max(max_fg - min_fg, max_bg - min_bg));
                            }
                        }

                        // choose the unicode char to print which minimises the diff
                        mindiff = 256;
                        case_min = 0;
                        for (int case_it = 0; case_it < static_cast<int>(std::size(cases)); case_it++) {
                            if (cases[case_it] < mindiff) {
                                case_min = case_it;
                                mindiff = cases[case_it];
                            }
                        }
                        shapechar = characters[case_min];

                        diffbg = 0;
                        diffpixel = 0;
                        bgsame = false;
                        pixelsame = false;

                        // based on the unicode character selected, find the avg colour of the pixels
                        // in the foreground region and background region
                        // the avg colour will be used as the colour to be printed
                        for (int k = 0; k < 3; k++) {
                            int bg_count = 0, fg_count = 0;
                            pixelchar[k] = 0;
                            pixelbg[k] = 0;
                            for (int i = 0; i < CHAR_Y; i++)
                                for (int j = 0; j < CHAR_X; j++) {
                                    if (pixelmap[case_min][i * CHAR_X + j]) {
                                        pixelchar[k] += pixel[i][j][k];
                                        fg_count++;
                                    } else {
                                        pixelbg[k] += pixel[i][j][k];
                                        bg_count++;
                                    }
                                }
                            pixelchar[k] /= fg_count;
                            pixelbg[k] /= bg_count;

                            // find the max diff between the foreground and background colours
                            // of the previously printed character
                            diffbg = std::max(diffbg, std::abs(pixelbg[k] - prevpixelbg[k]));
                            diffpixel = std::max(diffpixel, std::abs(pixelchar[k] - prevpixel[k]));
                        }

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
                                        *(oldrow[i] + (x * sx + j * skipx) * 3 + k) = (char) pixelchar[k];
                                    else
                                        *(oldrow[i] + (x * sx + j * skipx) * 3 + k) = (char) pixelbg[k];
                                }

                        // if the cursor is already in the right position, do not print the ansi move cursor command
                        // the ansi position command is one indexed
                        if (r != ay || c != x) {
                            print_ret = snprintf(print_buf + written, print_buffer_size - written,
                            "\x1B[%d;%dH", ay+1, x+1);
                            if (print_ret > 0 && print_ret < print_buffer_size - written)
                                written += print_ret;
                        }

                        // prints background and foreground colour change command, or either of them, or none
                        // depending on the previously computed difference
                        // color codes and character
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
            refresh = false;
            // print the fps, avg fps, dropped frames, etc. at the bottom of the video
            print_ret = snprintf(print_buf + written, print_buffer_size - written,
                                         "\x1B[%d;%dH\x1B[48;2;0;0;0;38;2;255;255;255m   fps:  %5.2f   |   avg_fps:  %5.2f   |   print:  %6.2fms   |   dropped:  %5d   |   curr_frame:  %5d                 ",
                                         msg_y+1, 1, static_cast<double>(frametimes.size()) * 1000000.0 / frame10_time, avg_fps,
                                         static_cast<double>(printing_time) / 1000.0, dropped, curr_frame);
            if (print_ret > 0 && print_ret < print_buffer_size - written)
                written += print_ret;

            printtime = std::chrono::steady_clock::now();
            // one write call for entire frame
            write(STDOUT_FILENO, print_buf, written);

            printing_time = (int) std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - printtime).count();
            total_printing_time += printing_time;
        }

        // free the buffers when the video completes
        if (alloc) {
            std::free(frame);
            std::free(old);
            std::free(print_buf);
        }
    } else {
        printf("\x1B[0mfile not found\n");
        fflush(stdout);
        exit(0);
    }
    terminateProgram(0);
    return 0;
}
