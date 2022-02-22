# terminalvideoplayer

This is a cursed terminal video player. This improves on some previous designs by implementing some optimisations such as not changing the text/background colour if the next pixel is similar enough. The video player also manages to get 4 pixels (effectively) out of every character as opposed to the usual 2 pixels by using the unicode quarter block characters.

Mileage may vary depending on how fast your terminal is. In my testing, I've found that [alacritty](https://github.com/alacritty/alacritty) works rather well.

```sh
.\tvp filename [threshold] 
```

The threshold has to be an integer from 0 to 255, and defaults to 10. The threshold affects how much the colour of a certain pixel has to change before it will be redrawn. A lower threshold results in more redraws in most cases, and leads to choppy video.

Built on Manjaro with this command:

```sh
g++ src/main.cpp -O3 -o ./tvp `pkg-config --cflags --libs opencv4`
```

Below is a preview of how it looks:

![video playing in terminal](imgs/video.gif)