//
// Created by orange on 24/2/22.
//

#include "video.h"

#include <queue>

struct AudioBuffer {
    std::queue<std::vector<uint8_t>> queue;
    std::mutex mutex;
    size_t max_size = 30; // around 0.5 seconds of audio buffered
};

static AudioBuffer audio_buffer;

// sdl audio callback
void audio_callback([[maybe_unused]] void* userdata, uint8_t* stream, int len) {
    std::lock_guard<std::mutex> lock(audio_buffer.mutex);

    int bytes_written = 0;
    while (bytes_written < len && !audio_buffer.queue.empty()) {
        auto& front = audio_buffer.queue.front();
        int to_copy = std::min((int)front.size(), len - bytes_written);

        memcpy(stream + bytes_written, front.data(), to_copy);
        bytes_written += to_copy;

        if (to_copy == (int)front.size()) {
            audio_buffer.queue.pop();
        } else {
            front.erase(front.begin(), front.begin() + to_copy);
        }
    }

    // Fill remaining with silence
    if (bytes_written < len) {
        memset(stream + bytes_written, 0, len - bytes_written);
    }
}

video::video(const char filename[], int w, int h) {
    errbuf[0] = '\0';
    av_log_set_level(AV_LOG_ERROR);
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

    // attempt to find audio stream
    ret = av_find_best_stream(inctx, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (ret >= 0) {
        astrm_idx = ret;
        astrm = inctx->streams[astrm_idx];

        audio_codec = avcodec_alloc_context3(acodec);
        ret = avcodec_parameters_to_context(audio_codec, astrm->codecpar);
        if (ret < 0) {
            av_make_error_string(errbuf, sizeof(errbuf), ret);
            fprintf(stderr, "fail to avcodec_parameters_to_context (audio): %s\n", errbuf);
            avcodec_free_context(&audio_codec);
            astrm_idx = -1;
        } else {
            ret = avcodec_open2(audio_codec, acodec, nullptr);
            if (ret < 0) {
                av_make_error_string(errbuf, sizeof(errbuf), ret);
                fprintf(stderr, "fail to avcodec_open2 (audio): %s\n", errbuf);
                avcodec_free_context(&audio_codec);
                astrm_idx = -1;
            } else {
                audio_frame = av_frame_alloc();
                audio_available = true;

                // init sdl audio
                if (SDL_Init(SDL_INIT_AUDIO) < 0) {
                    fprintf(stderr, "failed to init SDL audio: %s\n", SDL_GetError());
                    audio_available = false;
                } else {
                    SDL_AudioSpec wanted_spec, spec;
                    wanted_spec.freq = audio_codec->sample_rate;
                    wanted_spec.format = AUDIO_S16SYS;
                    wanted_spec.channels = audio_codec->ch_layout.nb_channels;
                    wanted_spec.silence = 0;
                    wanted_spec.samples = 1024;
                    wanted_spec.callback = audio_callback;
                    wanted_spec.userdata = this;

                    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
                        fprintf(stderr, "failed to open audio: %s\n", SDL_GetError());
                        audio_available = false;
                    } else {
                        // Setup resampler
                        int ret_swr = swr_alloc_set_opts2(&swr_ctx,
                            &audio_codec->ch_layout,
                            AV_SAMPLE_FMT_S16,
                            spec.freq,
                            &audio_codec->ch_layout,
                            audio_codec->sample_fmt,
                            audio_codec->sample_rate,
                            0, nullptr);

                        if (ret_swr < 0) {
                            fprintf(stderr, "failed to allocate resampler\n");
                            audio_available = false;
                            SDL_CloseAudio();
                        } else {
                            ret_swr = swr_init(swr_ctx);
                            if (ret_swr < 0) {
                                fprintf(stderr, "failed to initialize resampler\n");
                                audio_available = false;
                                SDL_CloseAudio();
                                swr_free(&swr_ctx);
                            } else {
                                swr_set_compensation(swr_ctx, 0, 0);
                                SDL_PauseAudio(0); // start playing
                            }
                        }
                    }
                }
            }
        }
    }

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

bool video::has_audio() const {
    return audio_available;
}

int video::get_audio_sample_rate() const {
    return audio_available ? audio_codec->sample_rate : 0;
}

int video::get_audio_channels() const {
    return audio_available ? audio_codec->ch_layout.nb_channels : 0;
}

int video::get_frame(int dst_w, int dst_h, const char* dst_frame) {
    int ret;
    bool got_video_frame = false;

    if (dst_w != dst_width || dst_h != dst_height) return 1;

    do {
        if (!end_of_stream_pkt) {
            ret = av_read_frame(inctx, pkt);
            end_of_stream_pkt = (AVERROR_EOF == ret);
            if (end_of_stream_pkt) {
                avcodec_send_packet(codec, nullptr);
                if (audio_available) avcodec_send_packet(audio_codec, nullptr);
            }
            if (ret < 0 && ret != AVERROR_EOF) {
                av_make_error_string(errbuf, sizeof(errbuf), ret);
                fprintf(stderr, "fail to av_read_frame: %s\n", errbuf);
                av_packet_unref(pkt);
                return -1;
            }
        }

        if (!end_of_stream_pkt) {
            if (pkt->stream_index == vstrm_idx) {
                ret = avcodec_send_packet(codec, pkt);
                if (ret < 0) {
                    av_make_error_string(errbuf, sizeof(errbuf), ret);
                    fprintf(stderr, "fail to av_send_packet: %s\n", errbuf);
                }
            } else if (audio_available && pkt->stream_index == astrm_idx) {
                // decode audio packet
                ret = avcodec_send_packet(audio_codec, pkt);
                if (ret < 0) {
                    av_make_error_string(errbuf, sizeof(errbuf), ret);
                    fprintf(stderr, "fail to av_send_packet (audio): %s\n", errbuf);
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(audio_codec, audio_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        av_make_error_string(errbuf, sizeof(errbuf), ret);
                        fprintf(stderr, "fail to av_receive_frame (audio): %s\n", errbuf);
                        break;
                    }

                    // check if we have valid audio data
                    if (!audio_frame->nb_samples || !swr_ctx) {
                        break;
                    }

                    // resample audio
                    int out_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, audio_codec->sample_rate) + audio_frame->nb_samples,
                        audio_codec->sample_rate,
                        audio_codec->sample_rate,
                        AV_ROUND_UP);

                    if (out_samples <= 0) break;

                    int nb_channels = audio_codec->ch_layout.nb_channels;
                    std::vector<uint8_t> audio_data(out_samples * nb_channels * 2);
                    uint8_t* out_ptr = audio_data.data();

                    int converted = swr_convert(swr_ctx,
                        &out_ptr,
                        out_samples,
                        (const uint8_t**)audio_frame->data,
                        audio_frame->nb_samples);

                    if (converted > 0) {
                        audio_data.resize(converted * nb_channels * 2);

                        std::lock_guard<std::mutex> lock(audio_buffer.mutex);
                        if (audio_buffer.queue.size() < audio_buffer.max_size) {
                            audio_buffer.queue.push(std::move(audio_data));
                        }
                    }
                }
            }
        }

        if (!got_video_frame) {
            ret = avcodec_receive_frame(codec, decframe);
            if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                av_make_error_string(errbuf, sizeof(errbuf), ret);
                fprintf(stderr, "fail to av_receive_frame: %s\n", errbuf);
            }
            end_of_stream_enc = (AVERROR_EOF == ret);

            if (ret == 0) {
                got_video_frame = true;
            }
        }

        av_packet_unref(pkt);
    } while(!got_video_frame && !end_of_stream_enc);

    if (end_of_stream_enc) return -1;

    sws_scale(swsctx, decframe->data, decframe->linesize, 0, decframe->height, frame->data, frame->linesize);
    av_image_copy_to_buffer((uint8_t *) dst_frame, get_dst_buf_size(), frame->data, frame->linesize, dst_pix_fmt, dst_width, dst_height, 1);

    return 0;
}

bool video::isOpened() const {
    return opened;
}

void video::setResize(int w, int h) {
    if (dst_width == w && dst_height == h) {
        return;  // no change required
    }

    dst_width = w;
    dst_height = h;

    // free old sws context
    if (swsctx) {
        sws_freeContext(swsctx);
        swsctx = nullptr;
    }

    swsctx = sws_getContext(
        codec->width, codec->height, codec->pix_fmt,
        dst_width, dst_height, dst_pix_fmt,
        SWS_BICUBIC, nullptr, nullptr, nullptr
    );

    if (!swsctx) {
        fprintf(stderr, "fail to sws_getContext\n");
        return;
    }

    // realloc frame buffer
    if (alloc && frame->data[0]) {
        av_freep(&frame->data[0]);
    }

    int ret = av_image_alloc(frame->data, frame->linesize,
                             dst_width, dst_height, dst_pix_fmt, 16);
    if (ret < 0) {
        fprintf(stderr, "fail to av_image_alloc\n");
        alloc = false;
        return;
    }
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

    if (audio_available) {
        SDL_CloseAudio();
        SDL_Quit();
        av_frame_free(&audio_frame);
        avcodec_free_context(&audio_codec);
        swr_free(&swr_ctx);
    }
}

int video::get_dst_buf_size() const {
    return dst_height*dst_width*3+50;
}

bool video::is_end_of_stream() const {
    return end_of_stream_enc;
}
