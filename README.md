# terminalvideoplayer

## Introduction

This is a cursed terminal video player. This improves on some previous designs by implementing some optimisations such as not changing the text/background colour if the next pixel is similar enough. The video player also manages to get 4 pixels (effectively) out of every character as opposed to the usual 2 pixels by using the unicode quarter block characters.

Mileage may vary depending on how fast your terminal is. In my testing, I've found that [alacritty](https://github.com/alacritty/alacritty) works rather well.

```sh
.\tvp filename [threshold] 
```

The threshold has to be an integer from 0 to 255, and defaults to 10. The threshold affects how much the colour of a certain pixel has to change before it will be redrawn. A lower threshold results in more redraws in most cases, and leads to choppy video. Unfortunately I can't be bothered to rewrite this to decode video using FFmpeg so you'll have to build it with OpenCV.

Built on Manjaro with this command:

```sh
g++ src/main.cpp -O3 -o tvp `pkg-config --cflags --libs opencv4`
```

Below is a preview of how it looks:

![video playing in terminal](./imgs/video.gif)

## How it works

This is not a new concept. But most terminal video players I have seen use two pixels per character. The unicode character  ▄  (U+2584 lower half block) as the bottom half of the pixel, which can be coloured using the ANSI code for font colour, and the background colour, as the other pixel.

But, if you search up the unicode block characters, you see this:

![unicode block characters table](./imgs/unicode_block_elements.png)

Clearly there is potential here. By using the unicode quarter block characters, as well as the half blocks

- ▘  (U+2598 quadrant upper left)
- ▝  (U+259D quadrant upper right)
- ▖  (U+2596 quadrant lower left)
- ▗  (U+2597 quadrant lower right)
- ▞  (U+259E 1uadrant upper right and lower left)
- ▄  (U+2584 lower half block)
- ▐  (U+2590 right half block)

We have all the building blocks for effectively double the horizontal resolution! However, we are still limited to two colours per pixel. So, we pick the configuration that minimizes the maximum difference between colours which will be made the same, and then set the colour to the average colour.

Other optimisations include 

- only changing pixels whose colour have changed a certain value
- only inputting the ANSI code for cursor move when the next pixel isnt contiguous
- only inputting the ANSI code for background colour change when the background colour differs significantly (set as a compile option)

