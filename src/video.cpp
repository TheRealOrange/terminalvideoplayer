//
// Created by orange on 24/2/22.
//

#include "video.h"

video::video(const char filename[], int w, int h) {
    errbuf[0] = '\0';
    int ret = avformat_open_input(&inctx, filename, nullptr, nullptr);
    if (ret < 0) {
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        fprintf(stderr, "fail to avformat_open_input(%s): %s\n", filename, errbuf);
        return;
    }

    ret = avformat_find_stream_info(inctx, nullptr);
    if (ret < 0) {
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        fprintf(stderr, "fail to avformat_find_stream_info: %s\n", errbuf);
        return;
    }
    ret = av_find_best_stream(inctx, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    if (ret < 0) {
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        fprintf(stderr, "fail to av_find_best_stream: %s\n", errbuf);
        return;
    }
    vstrm_idx = ret;
    vstrm = inctx->streams[vstrm_idx];

    codec = avcodec_alloc_context3(vcodec);
    ret = avcodec_parameters_to_context(codec, vstrm->codecpar);
    if (ret < 0) {
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        fprintf(stderr, "fail to avcodec_parameters_to_context: %s\n", errbuf);
        return;
    }

    ret = avcodec_open2(codec, vcodec, nullptr);
    if (ret < 0) {
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        fprintf(stderr, "fail to avcodec_open2: %s\n", errbuf);
        return;
    }

    src_width = codec->width;
    src_height = codec->height;

    if (w == -1 || h == -1) {
        dst_width = src_width;
        dst_height = src_height;
    } else {
        dst_width = w;
        dst_height = h;
    }

    frame = av_frame_alloc();
    decframe = av_frame_alloc();
    pkt = av_packet_alloc();

    setResize(dst_width, dst_height);

    opened = true;
}

double video::get_fps() const {
    return av_q2d(vstrm->r_frame_rate);
}

int video::get_width() const {
    return dst_width;
}

int video::get_height() const {
    return dst_height;
}

int video::get_frame(int dst_w, int dst_h, const char* dst_frame) {
    int ret;
    // read packet from input file
    if (dst_w != dst_width || dst_h != dst_height) return 1;
    do {
        if (!end_of_stream_pkt) {
            ret = av_read_frame(inctx, pkt);
            end_of_stream_pkt = (AVERROR_EOF == ret);
            if (end_of_stream_pkt) avcodec_send_packet(codec, nullptr);
            if (ret < 0 && ret != AVERROR_EOF) {
                av_make_error_string(errbuf, sizeof(errbuf), ret);
                fprintf(stderr, "fail to av_read_frame: %s\n", errbuf);
                av_packet_unref(pkt);
                return -1;
            }
        }

        if (!end_of_stream_pkt && pkt->stream_index == vstrm_idx) {
            ret = avcodec_send_packet(codec, pkt);
            if (ret < 0) {
                av_make_error_string(errbuf, sizeof(errbuf), ret);
                fprintf(stderr, "fail to av_send_packet: %s\n", errbuf);
            }
        }

        ret = avcodec_receive_frame(codec, decframe);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            av_make_error_string(errbuf, sizeof(errbuf), ret);
            fprintf(stderr, "fail to av_receive_frame: %s\n", errbuf);
        }
        end_of_stream_enc = (AVERROR_EOF == ret);

        av_packet_unref(pkt);
    } while(ret == AVERROR(EAGAIN) && !end_of_stream_enc);
    if (end_of_stream_enc) return -1;

    sws_scale(swsctx, decframe->data, decframe->linesize, 0, decframe->height, frame->data, frame->linesize);

    av_image_copy_to_buffer((uint8_t *) dst_frame, get_dst_buf_size(), frame->data, frame->linesize, dst_pix_fmt, dst_width, dst_height, 1);

    return 0;
}

bool video::isOpened() const {
    return opened;
}

void video::setResize(int w, int h) {
    dst_width = w;
    dst_height = h;
    swsctx = sws_getCachedContext(swsctx, codec->width, codec->height, codec->pix_fmt,
            dst_width, dst_height, dst_pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsctx) {
        fprintf(stderr, "fail to sws_getCachedContext\n");
        return;
    }

    if (alloc) av_freep(&frame->data[0]);
    av_image_alloc(frame->data, frame->linesize, dst_width, dst_height, dst_pix_fmt, 16);
    alloc = true;
}

video::~video() {
    av_freep(&frame->data);
    av_freep(&decframe->data);
    av_frame_free(&frame);
    av_frame_free(&decframe);
    av_packet_free(&pkt);
    avcodec_free_context(&codec);
    avformat_close_input(&inctx);
}

int video::get_dst_buf_size() const {
    return dst_height*dst_width*3+50;
}

bool video::is_end_of_stream() const {
    return end_of_stream_enc;
}
