#include "hffplayer.h"

#include "hlog.h"
#include "hstring.h"
#include "hscope.h"

std::atomic_flag HFFPlayer::s_init_flag;

void list_devices() {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    AVDictionary* options = NULL;
    av_dict_set(&options, "list_devices", "true", 0);
#ifdef _WIN32
    const char drive[] = "dshow";
#elif defined(__linux__)
    const char drive[] = "v4l2";
#else
    const char drive[] = "avfoundation";
#endif
    AVInputFormat* ifmt = av_find_input_format(drive);
    if (ifmt) {
        avformat_open_input(&fmt_ctx, "video=dummy", ifmt, &options);
    }
    avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
    av_dict_free(&options);
}

HFFPlayer::HFFPlayer()
: HVideoPlayer()
, HThread() {
    fmt_ctx = NULL;
    codec_ctx = NULL;
    packet = NULL;
    frame = NULL;

    if (!s_init_flag.test_and_set()) {
        av_register_all();
        avcodec_register_all();
        avdevice_register_all();
        list_devices();
    }
}

HFFPlayer::~HFFPlayer() {
    cleanup();
}

int HFFPlayer::start(){
    std::string ifile;

    AVInputFormat* ifmt = NULL;
    switch (media.type) {
    case MEDIA_TYPE_CAPTURE:
        {
            ifile = "video=";
            ifile += media.src;
#ifdef _WIN32
            const char drive[] = "dshow";
#elif defined(__linux__)
            const char drive[] = "v4l2";
#else
            const char drive[] = "avfoundation";
#endif
            ifmt = av_find_input_format(drive);
            if (ifmt == NULL) {
                hloge("Can not find dshow");
                return -5;
            }
        }
        break;
    case MEDIA_TYPE_FILE:
    case MEDIA_TYPE_NETWORK:
        ifile = media.src;
        break;
    default:
        return -10;
    }

    hlogi("ifile:%s", ifile.c_str());
    int iRet = 0;
    fmt_ctx = avformat_alloc_context();
    if (fmt_ctx == NULL) {
        hloge("avformat_alloc_context");
        return -10;
    }
    iRet = avformat_open_input(&fmt_ctx, ifile.c_str(), ifmt, NULL);
    if (iRet != 0) {
        hloge("Open input file[%s] failed: %d", ifile.c_str(), iRet);
        return iRet;
    }

    iRet = avformat_find_stream_info(fmt_ctx, NULL);
    if (iRet != 0) {
        hloge("Can not find stream: %d", iRet);
        return iRet;
    }

    hlogi("stream_num=%d", fmt_ctx->nb_streams);

    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    subtitle_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    hlogi("video_stream_index=%d", video_stream_index);
    hlogi("audio_stream_index=%d", audio_stream_index);
    hlogi("subtitle_stream_index=%d", subtitle_stream_index);

    if (video_stream_index < 0) {
        hloge("Can not find video stream.");
        return -20;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_index];
    video_time_base_num = video_stream->time_base.num;
    video_time_base_den = video_stream->time_base.den;
    if (video_stream->avg_frame_rate.num && video_stream->avg_frame_rate.den) {
        fps = video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    }
    if (video_time_base_num && video_time_base_den) {
        duration = video_stream->duration / (double)video_time_base_den * video_time_base_num * 1000;
        start_time = video_stream->start_time / (double)video_time_base_den * video_time_base_num * 1000;
    }
    hlogi("fps=%d duration=%lldms start_time=%lldms", fps, duration, start_time);

    AVCodecParameters* codecpar = video_stream->codecpar;
    hlogi("codec_id=%d:%s", codecpar->codec_id, avcodec_get_name(codecpar->codec_id));

    std::string cuvid(avcodec_get_name(codecpar->codec_id));
    cuvid += "_cuvid";
    hlogi("cuvid=%s", cuvid.c_str());

    AVCodec* codec = NULL;
    if (decode_mode == HARDWARE_DECODE && codec == NULL) {
        codec = avcodec_find_decoder_by_name(cuvid.c_str());
        if (codec == NULL) {
            hlogi("Can not find decoder %s!", cuvid.c_str());
            // no return, try soft-decoder
        }
    }

    if (codec == NULL) {
        codec = avcodec_find_decoder(codecpar->codec_id);
        if (codec == NULL) {
            hloge("Can not find decoder %s!", avcodec_get_name(codecpar->codec_id));
            return -20;
        }
    }

    hlogi("codec_name: %s=>%s", codec->name, codec->long_name);

    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == NULL) {
        hloge("avcodec_alloc_context3!");
        return -60;
    }

    iRet = avcodec_parameters_to_context(codec_ctx, codecpar);
    if (iRet != 0) {
        hloge("avcodec_parameters_to_context error: %d", iRet);
        return iRet;
    }

    iRet = avcodec_open2(codec_ctx, codec, NULL);
    if (iRet != 0) {
        hloge("Can not open codec: %d", iRet);
        return iRet;
    }

    int sw = codec_ctx->width;
    int sh = codec_ctx->height;
    AVPixelFormat src_pix_fmt = codec_ctx->pix_fmt;

    int dw = sw >> 2 << 2; // align = 4
    int dh = sh;

    hframe.w = dw;
    hframe.h = dh;

    dst_pix_fmt = AV_PIX_FMT_BGR24;
    hframe.type = GL_BGR;
    hframe.bpp = 24;
    hframe.buf.resize(dw * dh * hframe.bpp / 8);
    data[0] = (uint8_t*)hframe.buf.base;
    linesize[0] = dw * 3;

    // dst_pix_fmt = AV_PIX_FMT_YUV420P;
    // hframe.type = GL_I420;
    // hframe.bpp = 12;
    // int y_size = dw * dh;
    // hframe.buf.resize(y_size * 3 / 2);
    // data[0] = hframe.buf.base;
    // data[1] = data[0] + y_size;
    // data[2] = data[1] + y_size/4;
    // linesize[0] = dw;
    // linesize[1] = linesize[2] = dw / 2;

    frame = av_frame_alloc();
    packet = av_packet_alloc();
    sws_ctx = sws_getContext(sw, sh, src_pix_fmt, dw, dh, dst_pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);

    hlogi("sw=%d sh=%d dw=%d dh=%d", sw, sh, dw, dh);
    hlogi("src_pix_fmt=%d:%s dst_pix_fmt=%d:%s", src_pix_fmt, av_get_pix_fmt_name(src_pix_fmt),
        dst_pix_fmt, av_get_pix_fmt_name(dst_pix_fmt));

    HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000/fps);
    return HThread::start();
}

void HFFPlayer::cleanup() {
    if (codec_ctx) {
        avcodec_close(codec_ctx);
        avcodec_free_context(&codec_ctx);
        codec_ctx = NULL;
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }

    if (fmt_ctx) {
        avformat_free_context(fmt_ctx);
        fmt_ctx = NULL;
    }

    if (frame) {
        av_frame_unref(frame);
        av_frame_free(&frame);
        frame = NULL;
    }

    if (packet) {
        av_packet_unref(packet);
        av_packet_free(&packet);
        packet = NULL;
    }

    hframe.buf.cleanup();

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }
}

int HFFPlayer::stop(){
    HThread::stop();
    cleanup();
    return 0;
}

int HFFPlayer::seek(int64_t ms) {
    if (fmt_ctx) {
        hlogi("seek=>%lldms", ms);
        return av_seek_frame(fmt_ctx, video_stream_index,
                (start_time+ms)/1000/(double)video_time_base_num*video_time_base_den,
                AVSEEK_FLAG_BACKWARD);
    }
    return 0;
}

void HFFPlayer::doTask(){
    // loop until get a video frame
    while (1) {
        av_init_packet(packet);
        int iRet = av_read_frame(fmt_ctx, packet);
        if (iRet != 0) {
            hlogi("No frame: %d", iRet);
            signal = SIGNAL_END_OF_FILE;
            return;
        }

        if (packet->stream_index != video_stream_index) {
            continue;
        }

        int got_pic = 0;
        iRet = avcodec_decode_video2(codec_ctx, frame, &got_pic, packet);
        if (iRet < 0) {
            hloge("decoder error: %d", iRet);
            return;
        }

        if (got_pic)    break;  // exit loop
    }

    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, data, linesize);

    hframe.ts = frame->pts / (double)video_time_base_den * video_time_base_num * 1000;
    //hlogi("ts=%lldms", hframe.ts);

    push_frame(&hframe);
}
