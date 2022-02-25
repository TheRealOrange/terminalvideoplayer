//
// Created by orange on 24/2/22.
//

#ifndef VIDPLAYER_VIDEO_H
#define VIDPLAYER_VIDEO_H

#include <cstring>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

class video {
public:
    video(char filename[], int w, int h);
    explicit video(char filename[]) : video(filename, -1, -1) {};
    ~video();

    void setResize(int w, int h);
    [[nodiscard]] bool isOpened() const;
    double get_fps();
    [[nodiscard]] int get_width() const;
    [[nodiscard]] int get_height() const;
    [[nodiscard]] int get_dst_buf_size() const;
    [[nodiscard]] bool is_end_of_stream() const;
    int get_frame(int dst_w, int dst_h, const char* dst_frame);

private:
    AVFormatContext* inctx = nullptr;
    AVCodecContext* codec;
    AVCodec* vcodec = nullptr;
    AVStream* vstrm = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* swsctx = nullptr;
    bool opened = false;

    int vstrm_idx;

    int src_width;
    int src_height;
    int dst_width;
    int dst_height;

    AVFrame* decframe = nullptr;
    bool end_of_stream_pkt = false, end_of_stream_enc = false;
    AVPacket* pkt = nullptr;

    bool alloc = false;

    const AVPixelFormat dst_pix_fmt = AV_PIX_FMT_BGR24;
    char errbuf[200]{};
};


#endif //VIDPLAYER_VIDEO_H
