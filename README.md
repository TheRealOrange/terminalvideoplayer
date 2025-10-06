# terminalvideoplayer

### note:
For some reason unbeknownst to me I have decided, against my good judgement, to invest even 
more time into this project, this time with a healthy splash of _LLM-assisted modifications_
(gasp, shock, horror). In any case, I have improved this video player in various ways, such 
as by adding OpenCL acceleration and separating printing into its own thread.

## Introduction

This is a cursed terminal video player. This improves on some previous designs by implementing some optimisations such
as not changing the text/background colour if the next pixel is similar enough. The video player also manages to get
more "pixels" (effectively) out of every character as opposed to the usual 2 pixels by using the unicode quarter block
characters. The pixels however aren't really independent, and each character is still limited to two colours.

Mileage may vary depending on how fast your terminal is. In my testing, I've found
that [alacritty](https://github.com/alacritty/alacritty) works rather well. (As of 2025, I can say that iTerm2 also performs spectacularly well)

## Features

- Audio playback, using SDL2
- Expanded characterset of 44 (OpenCL) or 22 (CPU) Unicode characters for better representation of the image
- OpenCL acceleration for systems with supported devices to render the frames using the expanded characterset
- Perceptual color optimisation using either fast weighted RGB or Oklab color space
- Resizable terminal video playback

## Usage
```sh
Usage: tvp <video_file> [diff_threshold] [options]
Options:
  --force-cpu     Force CPU computation
  --no-audio      Disable audio playback
  --print-usage   Print character usage rates
  --help          Show this help message
```

The threshold has to be an integer from 0 to 255, and defaults to 10. The threshold affects how much the colour of a
certain pixel has to change before it will be redrawn. A lower threshold results in more redraws in most cases, and
leads to choppy video (especially on slower terminals). It relies on [ffmpeg](https://www.ffmpeg.org/) in order to decode the video input.

Build has been tested on macOS and Windows, albeit with some caveats on Windows (dynamic/static linking on windows 
is fickle and finnicky, builds with Visual Studio differ from MinGW, `vcpckg` issues, etc.).

### Dependencies
- FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample)
- SDL2 (for audio playback)
- OpenCL (optional, for GPU acceleration)

Below is a preview of how it looks with full color video, note the size/scale of the terminal (compare to the text):

![video playing in terminal](./imgs/colorvid.gif)

And of course, as is custom (at this point it is basically canon for any esoteric display format to play), 
this is how it looks with bad apple video:

![video playing in terminal](./imgs/badapple.gif)


## How it works

This is not a new concept. But most terminal video players I have seen use two pixels per character. The unicode
character ▄  (U+2584 lower half block) as the bottom half of the pixel, which can be coloured using the ANSI code for
font colour, and the background colour, as the other pixel.

But, if you search up the unicode block characters, you see this:

![unicode block characters table](./imgs/unicode_block_elements.png)

Clearly there is potential here. By using the unicode quarter block characters, as well as the half blocks

- ▘  (U+2598 quadrant upper left)
- ▝  (U+259D quadrant upper right)
- ▖  (U+2596 quadrant lower left)
- ▗  (U+2597 quadrant lower right)
- ▞  (U+259E quadrant upper right and lower left)
- ▄  (U+2584 lower half block)
- ▐  (U+2590 right half block)
- ▂  (U+2582 lower quarter block)
- ▆  (U+2586 lower 3 quarters block)
- ▎  (U+258E left quarter block)
- ▊  (U+258A left 3 quarters block)

(Updated to use the following additional characters)

**CPU and OpenCL:**
- ▁  (U+2581 lower 1/8 block)
- ▃  (U+2583 lower 3/8 block)
- ▅  (U+2585 lower 5/8 block)
- ▇  (U+2587 lower 7/8 block)
- ▏  (U+258F left 1/8 vertical)
- ▍  (U+258D left 3/8 vertical)
- ▋  (U+258B left 5/8 vertical)
- ▉  (U+2589 left 7/8 vertical)
- ━  (U+2501 thick horizontal middle line)
- ┃  (U+2503 thick vertical center line)
- ■  (U+25A0 center square)

**OpenCL only (excluded from CPU for performance):**
- ▪  (U+25AA center square with space)
- ▮  (U+25AE center rectangle with space)
- ◀  (U+25C0 left pointing triangle)
- ▶  (U+25B6 right pointing triangle)
- ◢  (U+25E2 lower right triangle)
- ◣  (U+25E3 lower left triangle)
- ◤  (U+25E4 upper left triangle)
- ◥  (U+25E5 upper right triangle)
- ┏  (U+250F down and right - top-left corner)
- ┓  (U+2513 down and left - top-right corner)
- ┗  (U+2517 up and right - bottom-left corner)
- ┛  (U+251B up and left - bottom-right corner)
- ▲  (U+25B2 upwards pointing triangle)
- ▼  (U+25BC downwards pointing triangle)
- ┣  (U+2523 vertical and right - left T-junction)
- ┫  (U+252B vertical and left - right T-junction)
- ┳  (U+2533 down and horizontal - top T-junction)
- ┻  (U+253B up and horizontal - bottom T-junction)
- ╋  (U+254B vertical and horizontal - cross junction)
- ╱  (U+2571 bottom left to top right diagonal)
- ╲  (U+2572 bottom right to top left diagonal)
- ╳  (U+2573 corner to corner cross shape)

We have all the building blocks for effectively kind of quadruple the horizontal and double the vertical resolution!
However, we are still limited to two colours per pixel. So, we pick the configuration that minimizes the maximum
difference between colours which will be made the same, and then set the colour to the average colour.

Other optimisations include

- only changing pixels whose colour have changed a certain value
- only inputting the ANSI code for cursor move when the next pixel isn't contiguous
- only inputting the ANSI code for background colour change when the background colour differs significantly (set as a
  compile option)

