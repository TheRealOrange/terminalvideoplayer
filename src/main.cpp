#include <opencv2/opencv.hpp>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#include <queue>
#include <csignal>
#include <cstdlib>

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
                                        "\u2586",
                                        "\u258e",
                                        "\u258a"};// lower 3 quarters block

const int pixelmap[DIFF_CASES][CHAR_Y*CHAR_X] = {{0, 0, 0, 0,
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

using namespace cv;

void get_terminal_size(int &width, int &height) {
    width = -1; height = -1;
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

int w = -1, h = -1;
int im_w, im_h;
double scale_factor = 0.0;
int small_dims[2];

Mat frame;
Mat resized;
Mat old;

int diff = 0;
int pixel[CHAR_Y][CHAR_X][3];
bool refresh = false;
bool begin = true;

int r, c;

char printbuf[100000000];
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

int curr_w, curr_h, orig_w = -1, orig_h = -1;

int mindiff, diffbg, diffpixel;
int min_fg, min_bg, max_fg, max_bg;
int cases[DIFF_CASES];
int case_min = 0;

bool bgsame = false, pixelsame = false;

int prevpixelbg[3] = {1000, 1000, 1000};
int pixelbg[3], pixelchar[3];
int prevpixel[3] = {1000, 1000, 1000};

int sx = 4, sy = 8;
int skipy = sy/CHAR_Y, skipx = sx/CHAR_X;

void terminateProgram([[maybe_unused]] int sig_num) {
    videostop = std::chrono::steady_clock::now();
    long long total_video_time = (long long) std::chrono::duration_cast<std::chrono::microseconds>(
            videostop - videostart).count();
    printf("\u001b[0m\u001b[%d;%dHframes: %5d, dropped: %5d,  total time: %5.2fs,  printing time: %5.2fs                                                            \u001b[?25h",
           msg_y, 0, curr_frame, dropped, (double) total_video_time / 1000000.0,
           (double) total_printing_time / 1000000.0);
    fflush(stdout);
    exit(0);
}

int main(int argc, char *argv[]) {
    videostart = std::chrono::steady_clock::now();
    signal(SIGINT, terminateProgram);
    setvbuf(stdout, printbuf, _IOLBF, sizeof(printbuf));
    // Create a VideoCapture object and open the input file
    // If the input is the web camera, pass 0 instead of the video file name
    if (argc <= 1 || strlen(argv[1]) <= 0) {
        printf("\u001b[0mplease provide the filename as the first input argument");
        fflush(stdout);
        return 0;
    }
#if defined(__APPLE__)
    if (std::__fs::filesystem::exists(argv[1])) {
#else
    if (std::filesystem::exists(argv[1])) {
#endif
        if (argc > 2) diffthreshold = std::stoi(argv[2], nullptr, 10);
        diffthreshold = std::max(std::min(255, diffthreshold), 0);
        VideoCapture cap(argv[1]);
        // Check if camera opened successfully
        if (!cap.isOpened()) {
            printf("\u001b[0mError opening video stream or file\n");
            fflush(stdout);
            return -1;
        }

        fps = cap.get(CAP_PROP_FPS);
        period = (int) (1000000.0 / fps);
        start = std::chrono::steady_clock::now();
        while (true) {
            count++;
            curr_frame++;
            // Capture frame-by-frame
            cap >> frame;

            get_terminal_size(curr_w, curr_h);
            if (curr_w != orig_w || curr_h != orig_h) {
                orig_w = curr_w;
                orig_h = curr_h;
                w = curr_w;
                h = curr_h;
                h -= 1;
                msg_y = h;
                h *= sy;
                w *= sx;
                im_w = frame.cols;
                im_h = frame.rows;
                scale_factor = std::min((double) w / (double) im_w, (double) h / (double) im_h);
                small_dims[0] = int((double) im_w * scale_factor);
                small_dims[1] = int((double) im_h * scale_factor);
                if (small_dims[0] == 0 || small_dims[1] == 0) {
                    printf("\u001b[%d;%dHterminal dimensions is too small! (%d, %d)                                                                    \n",
                           msg_y, 0, curr_w, curr_h);
                    fflush(stdout);
                    exit(0);
                }
                refresh = true;
                if (begin) {
                    begin = false;
                    printf("\u001b[?25l");
                    printf("terminal dimensions: (w %4d, h %4d)\n", curr_w, curr_h);
                    printf("frame dimensions:    (w %4d, h %4d)\n", im_w, im_h);
                    printf("display dimensions:  (w %4d, h %4d)\n", small_dims[0], small_dims[1] / (sy/sx));
                    printf("scaling:             %f\n", scale_factor);
                    printf("frames per second:   %f\n", fps);
                    fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    start = std::chrono::steady_clock::now();
                    videostart = std::chrono::steady_clock::now();
                }
                printf("\u001b[0;0H\u001b[48;2;0;0;0m");
                for (int i = 0; i < curr_w * curr_h; i++) printf(" ");
            }

            stop = std::chrono::steady_clock::now();
            elapsed = (int)std::chrono::duration_cast<std::chrono::microseconds>(stop - videostart).count();
            int frame_time = (int)std::chrono::duration_cast<std::chrono::microseconds>(
                    stop - start).count();
            start = std::chrono::steady_clock::now();

            total_time = elapsed;
            avg_fps = (double) count * 1000000.0 / (double) total_time;
            frametimes.push(frame_time);
            frame10_time += frame_time;
            if (frametimes.size() > 10) {
                frame10_time -= frametimes.front();
                frametimes.pop();
            }

            if (curr_frame * period - elapsed > 0)
                std::this_thread::sleep_until(std::chrono::microseconds(curr_frame * period - elapsed - frame10_time/frametimes.size())+stop);
            else {
                skip = (double) elapsed / (double) period - (double) curr_frame;
                for (int i = 0; i < std::floor(skip); i++) cap >> frame;
                dropped += std::floor(skip);
                curr_frame += std::floor(skip);
                std::this_thread::sleep_until(std::chrono::microseconds(curr_frame * period - frame10_time/frametimes.size()) + videostart);
            }

            printf("\u001b[%d;%dH\u001b[48;2;0;0;0;38;2;255;255;255m   fps:  %5.2f   |   avg_fps:  %5.2f   |   print:  %6.2fms   |   dropped:  %5d   |   curr_frame:  %5d                 ",
                   msg_y, 0, (double) frametimes.size() * 1000000.0 / frame10_time, avg_fps,
                   (double) printing_time / 1000.0, dropped, curr_frame);
            prevpixelbg[0] = 1000;
            prevpixelbg[1] = 1000;
            prevpixelbg[2] = 1000;
            prevpixel[0] = 1000;
            prevpixel[1] = 1000;
            prevpixel[2] = 1000;

            // If the frame is empty, break immediately
            if (frame.empty()) {
                printf("\u001b[0mError reading video stream or file\n");
                break;
            }

            resize(frame, resized, Size(small_dims[0], small_dims[1]), INTER_LINEAR);

            if (refresh) old = resized.clone();

            r = -1;
            c = -1;
            Vec3b *row[CHAR_Y];
            Vec3b *oldrow[CHAR_Y];
            for (int ay = 0; ay < resized.rows / sy; ay++) {
                for (int x = 0; x < resized.cols / sx; x++) {
                    for (int i = 0; i < CHAR_Y; i++) {
                        row[i] = resized.ptr<Vec3b>(ay * sy + i*skipy);
                        oldrow[i] = old.ptr<Vec3b>(ay * sy + i*skipy);
                    }
                    for (int i = 0; i < CHAR_Y; i++)
                        for (int j = 0; j < CHAR_X; j++)
                            for (int k = 0; k < 3; k++)
                                pixel[i][j][k] = row[i][x * sx + j*skipx][k];

                    diff = 0;
                    if (refresh) {
                        diff = 255;
                    } else {
                        for (int i = 0; i < CHAR_Y; i++)
                            for (int j = 0; j < CHAR_X; j++)
                                for (int k = 0; k < 3; k++)
                                    diff = std::max(diff, std::abs(oldrow[i][x * sx + j*skipx][k] - pixel[i][j][k]));
                    }

                    if (diff >= diffthreshold) {
                        for (int & case_it : cases) case_it = 0;

                        for (int k = 0; k < 3;k++) {
                            for (int case_it = 0;case_it < sizeof(cases)/sizeof(cases[0]);case_it++) {
                                min_fg = 256; min_bg = 256; max_fg = 0; max_bg = 0;
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

                        mindiff = 256;
                        case_min = 0;
                        for (int case_it = 0;case_it < sizeof(cases)/sizeof(cases[0]);case_it++) {
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

                        for (int k = 0; k < 3; k++) {
                            int bg_count = 0, fg_count = 0;
                            pixelchar[k] = 0; pixelbg[k] = 0;
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
                            diffbg = std::max(diffbg, std::abs(pixelbg[k] - prevpixelbg[k]));
                            diffpixel = std::max(diffpixel, std::abs(pixelchar[k] - prevpixel[k]));
                        }

                        if (diffbg < CHANGE_THRESHOLD) {
                            for (int k = 0;k < 3;k++) pixelbg[k] = prevpixelbg[k];
                            bgsame = true;
                        } else
                            for (int k = 0;k < 3;k++) prevpixelbg[k] = pixelbg[k];
                        if (diffpixel < CHANGE_THRESHOLD) {
                            for (int k = 0;k < 3;k++) pixelchar[k] = prevpixel[k];
                            pixelsame = true;
                        } else
                            for (int k = 0;k < 3;k++) prevpixel[k] = pixelchar[k];

                        for (int k = 0; k < 3; k++)
                            for (int i = 0; i < CHAR_Y; i++)
                                for (int j = 0; j < CHAR_X; j++) {
                                    if (pixelmap[case_min][i * CHAR_X + j])
                                        oldrow[i][x * sx + j*skipx][k] = pixelchar[k];
                                    else
                                        oldrow[i][x * sx + j*skipx][k] = pixelbg[k];
                                }

                        if (r != ay || c != x) {
                            printf("\u001b[%d;%dH", ay, x);
                        }

                        if (!bgsame && !pixelsame)
                            printf("\u001b[48;2;%d;%d;%d;38;2;%d;%d;%dm%s", pixelbg[2], pixelbg[1], pixelbg[0],
                                   pixelchar[2], pixelchar[1], pixelchar[0], shapechar);
                        else if (!bgsame)
                            printf("\u001b[48;2;%d;%d;%dm%s", pixelbg[2], pixelbg[1], pixelbg[0], shapechar);
                        else if (!pixelsame)
                            printf("\u001b[38;2;%d;%d;%dm%s", pixelchar[2], pixelchar[1], pixelchar[0], shapechar);
                        else
                            printf("%s", shapechar);

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
            printtime = std::chrono::steady_clock::now();
            fflush(stdout);

            printing_time = (int) std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - printtime).count();
            total_printing_time += printing_time;
        }

        // When everything done, release the video capture object
        cap.release();
    } else {
        printf("\u001b[0mfile not found\n");
        fflush(stdout);
        exit(0);
    }
    terminateProgram(0);
    return 0;
}
