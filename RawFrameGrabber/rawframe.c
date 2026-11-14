//
//  rawframe.c
//  IINA RAW
//
//  See rawframe.h. Every FFmpeg object is released on every path (see the `done` label).
//

#include "rawframe.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Decoder options of IINA RAW's playback (iina/AppData.swift), plus four slice threads.
// Playback needs real time at the clip's frame rate (25 fps): six slice threads give about 31 fps.
#define RAWFRAME_PLAYER_THREADS 6
#ifndef RAWFRAME_THREADS
#define RAWFRAME_THREADS 4
#endif
// A stalled volume must not keep a Quick Look extension busy for ever: all reading stops
// this many microseconds after the clip was opened.
#define RAWFRAME_DEADLINE_US (10 * 1000000LL)
// A damaged packet or frame is skipped; the attempt fails after this many of them.
#define RAWFRAME_MAX_INVALID 3

typedef struct Decoded {
  int64_t deadline;  // now_us() value after which reading is stopped
  AVFormatContext *fmt;
  AVCodecContext *codec;
  AVFrame *frame;
  int stream_index;
} Decoded;

static void set_error(char *error, size_t size, const char *what, int averr) {
  if (!error || size == 0) return;
  if (averr < 0) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(averr, text, sizeof(text));
    snprintf(error, size, "%s: %s", what, text);
  } else {
    snprintf(error, size, "%s", what);
  }
}

/// Monotonic clock in microseconds.
static int64_t now_us(void) {
  return (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000);
}

static int interrupt_cb(void *opaque) {
  const Decoded *d = (const Decoded *)opaque;
  return now_us() > d->deadline;
}

static void decoded_free(Decoded *d) {
  av_frame_free(&d->frame);
  avcodec_free_context(&d->codec);
  avformat_close_input(&d->fmt);
}

/// Open the clip and decode one frame at `fraction` of the duration. When the seek fails or
/// gives no frame, the first frame is used. Returns 0 on success; `d` then holds the open
/// objects and the caller releases them with decoded_free. On failure `d` is already released.
static int decode_one(const char *path, double fraction, int threads, Decoded *d, char *error, size_t error_size) {
  memset(d, 0, sizeof(*d));
  AVDictionary *options = NULL;
  AVPacket *packet = NULL;
  int ret = 0;
  int ok = 0;

  d->deadline = now_us() + RAWFRAME_DEADLINE_US;
  d->fmt = avformat_alloc_context();
  if (!d->fmt) { set_error(error, error_size, "out of memory", 0); goto done; }
  d->fmt->interrupt_callback.callback = interrupt_cb;
  d->fmt->interrupt_callback.opaque = d;
  ret = avformat_open_input(&d->fmt, path, NULL, NULL);
  if (ret < 0) { set_error(error, error_size, "cannot open the clip", ret); goto done; }
  ret = avformat_find_stream_info(d->fmt, NULL);
  if (ret < 0) { set_error(error, error_size, "cannot read the stream info", ret); goto done; }

  d->stream_index = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (d->stream_index < 0) { set_error(error, error_size, "the clip has no video stream", d->stream_index); goto done; }
  AVStream *stream = d->fmt->streams[d->stream_index];
  if (stream->codecpar->codec_id != AV_CODEC_ID_TICO_RAW) {
    set_error(error, error_size, "the video stream is not a Nikon raw (tico_raw) stream", 0);
    goto done;
  }
  const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) { set_error(error, error_size, "the tico_raw decoder is not in this FFmpeg build", 0); goto done; }

  d->codec = avcodec_alloc_context3(codec);
  if (!d->codec) { set_error(error, error_size, "out of memory", 0); goto done; }
  ret = avcodec_parameters_to_context(d->codec, stream->codecpar);
  if (ret < 0) { set_error(error, error_size, "cannot copy the codec parameters", ret); goto done; }
  d->codec->pkt_timebase = stream->time_base;
  d->codec->thread_count = threads;
  av_dict_set(&options, "view", "rec709", 0);
  av_dict_set(&options, "view_size", "half", 0);
  av_dict_set(&options, "thread_type", "slice", 0);
  ret = avcodec_open2(d->codec, codec, &options);
  if (ret < 0) { set_error(error, error_size, "cannot open the decoder", ret); goto done; }

  d->frame = av_frame_alloc();
  packet = av_packet_alloc();
  if (!d->frame || !packet) { set_error(error, error_size, "out of memory", 0); goto done; }

  // Seek to the wanted position; when that fails, stay at the start (frame 0).
  if (fraction > 0 && d->fmt->duration > 0) {
    double f = fraction > 1 ? 1 : fraction;
    int64_t start = d->fmt->start_time == AV_NOPTS_VALUE ? 0 : d->fmt->start_time;
    int64_t target = start + (int64_t)(f * (double)d->fmt->duration);
    if (av_seek_frame(d->fmt, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
      av_seek_frame(d->fmt, -1, start, AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(d->codec);
  }

  int invalid = 0;
  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
    int eof = 0;
    while (!ok && !eof) {
      if (now_us() > d->deadline) { ret = AVERROR_EXIT; break; }
      ret = av_read_frame(d->fmt, packet);
      if (ret < 0) {
        if (ret == AVERROR_EXIT) break;  // the deadline stopped the reading
        eof = 1;
        avcodec_send_packet(d->codec, NULL);  // drain
      } else if (packet->stream_index != d->stream_index) {
        av_packet_unref(packet);
        continue;
      } else {
        ret = avcodec_send_packet(d->codec, packet);
        av_packet_unref(packet);
        if (ret == AVERROR_INVALIDDATA && invalid < RAWFRAME_MAX_INVALID) { invalid++; continue; }  // skip a damaged packet
        if (ret < 0 && ret != AVERROR(EAGAIN)) break;
      }
      ret = avcodec_receive_frame(d->codec, d->frame);
      if (ret == 0) ok = 1;
      else if (ret == AVERROR_INVALIDDATA && invalid < RAWFRAME_MAX_INVALID) { invalid++; continue; }  // skip a damaged frame
      else if (ret != AVERROR(EAGAIN)) break;
    }
    if (!ok && attempt == 0 && fraction > 0 && ret != AVERROR_EXIT) {
      // The seek gave no frame: try again from the first frame.
      av_seek_frame(d->fmt, -1, d->fmt->start_time == AV_NOPTS_VALUE ? 0 : d->fmt->start_time, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(d->codec);
    } else {
      break;
    }
  }
  if (!ok) set_error(error, error_size, "no frame could be decoded", ret < 0 ? ret : 0);

done:
  av_dict_free(&options);
  av_packet_free(&packet);
  if (!ok) decoded_free(d);
  return ok ? 0 : -1;
}

/// Scale `frame` into `dst` (dw x dh, `dst_fmt`) with the scaler `*ctx`. The size and the pixel
/// format of a frame can change from one frame to the next in a damaged or crafted file, and a
/// scaler that was built for other values reads outside the frame. So the scaler is checked
/// against this frame every time (sws_getCachedContext builds a new one when width, height or
/// format differ) and a frame that is empty or has an unusable format is refused.
/// Returns 0 on success, -1 on failure (message in `error`).
static int scale_frame(struct SwsContext **ctx, const AVFrame *frame, int dw, int dh,
                       enum AVPixelFormat dst_fmt, int flags, uint8_t *const dst[4], const int dst_stride[4],
                       char *error, size_t error_size) {
  if (!frame || frame->width < 1 || frame->height < 1 || !frame->data[0]) {
    set_error(error, error_size, "the decoded frame is empty", 0);
    return -1;
  }
  const enum AVPixelFormat fmt = (enum AVPixelFormat)frame->format;
  if (frame->format < 0 || !sws_isSupportedInput(fmt)) {
    set_error(error, error_size, "the decoded frame has a pixel format that cannot be scaled", 0);
    return -1;
  }
  *ctx = sws_getCachedContext(*ctx, frame->width, frame->height, fmt, dw, dh, dst_fmt, flags, NULL, NULL, NULL);
  if (!*ctx) {
    set_error(error, error_size, "cannot create the scaler for the decoded frame", 0);
    return -1;
  }
  if (sws_scale(*ctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                (uint8_t *const *)dst, dst_stride) <= 0) {
    set_error(error, error_size, "the scaler failed", 0);
    return -1;
  }
  return 0;
}

static int parse_doubles(const char *text, double *values, int count);
static void fill_info(const Decoded *d, RawFrameInfo *out);

int rawframe_grab(const char *path, int max_pixel_size, double fraction,
                  RawFrameImage *out, char *error, size_t error_size) {
  return rawframe_grab_with_info(path, max_pixel_size, fraction, out, NULL, error, error_size);
}

int rawframe_grab_with_info(const char *path, int max_pixel_size, double fraction,
                            RawFrameImage *out, RawFrameInfo *info, char *error, size_t error_size) {
  if (!path || !out || max_pixel_size < 1) {
    set_error(error, error_size, "bad arguments", 0);
    return -1;
  }
  memset(out, 0, sizeof(*out));
  Decoded d;
  if (decode_one(path, fraction, RAWFRAME_THREADS, &d, error, error_size) < 0) return -1;

  int result = -1;
  struct SwsContext *sws = NULL;
  uint8_t *buffer = NULL;
  const int sw = d.frame->width, sh = d.frame->height;
  if (sw < 1 || sh < 1) { set_error(error, error_size, "the decoded frame is empty", 0); goto done; }

  // Scale so that the long side is at most max_pixel_size; never enlarge.
  double scale = (double)max_pixel_size / (double)(sw > sh ? sw : sh);
  if (scale > 1.0) scale = 1.0;
  int dw = (int)lround(sw * scale), dh = (int)lround(sh * scale);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;

  size_t stride = (size_t)dw * 4;
  buffer = (uint8_t *)malloc(stride * (size_t)dh);
  if (!buffer) { set_error(error, error_size, "out of memory", 0); goto done; }
  uint8_t *dst[4] = { buffer, NULL, NULL, NULL };
  int dst_stride[4] = { (int)stride, 0, 0, 0 };
  if (scale_frame(&sws, d.frame, dw, dh, AV_PIX_FMT_BGR0, SWS_BICUBIC, dst, dst_stride, error, error_size) < 0) goto done;
  out->width = dw;
  out->height = dh;
  out->stride = stride;
  out->data = buffer;
  buffer = NULL;
  if (info) fill_info(&d, info);
  result = 0;

done:
  free(buffer);
  sws_freeContext(sws);
  decoded_free(&d);
  return result;
}

static int parse_doubles(const char *text, double *values, int count) {
  const char *p = text;
  for (int i = 0; i < count; i++) {
    char *end = NULL;
    values[i] = strtod(p, &end);
    if (end == p) return 0;
    p = end;
  }
  return 1;
}

/// A duration from a damaged file can be infinite, NaN, negative or huge: keep it in 0...24 h.
static double clean_seconds(double v) {
  if (!isfinite(v) || v < 0) return 0;
  return v > 86400.0 ? 86400.0 : v;
}

/// Fill `out` from the decoded frame and the stream of `d`.
static void fill_info(const Decoded *d, RawFrameInfo *out) {
  memset(out, 0, sizeof(*out));
  AVStream *stream = d->fmt->streams[d->stream_index];
  out->width = d->frame->width;
  out->height = d->frame->height;
  AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
  double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0;
  out->fps = isfinite(fps) && fps >= 0.001 && fps <= 1000.0 ? fps : 0;
  if (d->fmt->duration > 0) out->duration_seconds = clean_seconds((double)d->fmt->duration / AV_TIME_BASE);
  else if (stream->duration > 0) out->duration_seconds = clean_seconds((double)stream->duration * av_q2d(stream->time_base));
  const AVCodecDescriptor *desc = avcodec_descriptor_get(stream->codecpar->codec_id);
  snprintf(out->codec_long_name, sizeof(out->codec_long_name), "%s",
           desc && desc->long_name ? desc->long_name : (d->codec->codec ? d->codec->codec->long_name : "tico_raw"));
  AVDictionaryEntry *e = av_dict_get(d->frame->metadata, "ticoraw.iso", NULL, 0);
  if (e && e->value) {
    double v = 0;
    // A damaged value (NaN, infinite, negative or huge) is left out.
    if (parse_doubles(e->value, &v, 1) && isfinite(v) && v >= 0 && v <= 1e7) { out->has_iso = 1; out->iso = (int)lround(v); }
  }
  e = av_dict_get(d->frame->metadata, "ticoraw.wb_gain", NULL, 0);
  double wb[3];
  if (e && e->value && parse_doubles(e->value, wb, 3) && isfinite(wb[0]) && isfinite(wb[1]) && isfinite(wb[2])) {
    memcpy(out->wb_gain, wb, sizeof(wb));
    out->has_wb_gain = 1;
  }
}

int rawframe_info(const char *path, RawFrameInfo *out, char *error, size_t error_size) {
  if (!path || !out) {
    set_error(error, error_size, "bad arguments", 0);
    return -1;
  }
  memset(out, 0, sizeof(*out));
  Decoded d;
  if (decode_one(path, 0.0, RAWFRAME_THREADS, &d, error, error_size) < 0) return -1;
  fill_info(&d, out);
  decoded_free(&d);
  return 0;
}

void rawframe_free_image(RawFrameImage *image) {
  if (!image) return;
  free(image->data);
  memset(image, 0, sizeof(*image));
}

// ---- playback -------------------------------------------------------------------------------

struct RawFramePlayer {
  Decoded d;
  AVPacket *packet;
  struct SwsContext *sws;
  int width, height;
  int pending;  // d.frame holds a frame that was not given out yet
};

RawFramePlayer *rawframe_player_open(const char *path, int max_pixel_size, int *width, int *height,
                                     RawFrameInfo *info, char *error, size_t error_size) {
  if (!path || max_pixel_size < 1) { set_error(error, error_size, "bad arguments", 0); return NULL; }
  RawFramePlayer *p = (RawFramePlayer *)calloc(1, sizeof(*p));
  if (!p) { set_error(error, error_size, "out of memory", 0); return NULL; }
  if (decode_one(path, 0.0, RAWFRAME_PLAYER_THREADS, &p->d, error, error_size) < 0) { free(p); return NULL; }
  const int sw = p->d.frame->width, sh = p->d.frame->height;
  if (sw < 1 || sh < 1) {
    set_error(error, error_size, "the decoded frame is empty", 0);
    rawframe_player_close(p);
    return NULL;
  }
  p->packet = av_packet_alloc();
  double scale = (double)max_pixel_size / (double)(sw > sh ? sw : sh);
  if (scale > 1.0) scale = 1.0;
  p->width = (int)lround(sw * scale);
  p->height = (int)lround(sh * scale);
  if (p->width < 1) p->width = 1;
  if (p->height < 1) p->height = 1;
  if (!p->packet) {
    set_error(error, error_size, "out of memory", 0);
    rawframe_player_close(p);
    return NULL;
  }
  p->pending = 1;
  if (info) fill_info(&p->d, info);
  if (width) *width = p->width;
  if (height) *height = p->height;
  return p;
}

/// Decode the next frame into p->d.frame. At the end of the clip it goes back to the start.
static int player_decode_next(RawFramePlayer *p, char *error, size_t error_size) {
  Decoded *d = &p->d;
  int invalid = 0, restarts = 0;
  for (;;) {
    d->deadline = now_us() + RAWFRAME_DEADLINE_US;
    int ret = av_read_frame(d->fmt, p->packet);
    if (ret == AVERROR_EOF || (ret < 0 && ret != AVERROR_EXIT && avio_feof(d->fmt->pb))) {
      // End of the clip: loop. Two restarts in a row without a frame mean the clip is empty.
      if (++restarts > 2) { set_error(error, error_size, "no frame after a restart", 0); return -1; }
      avcodec_flush_buffers(d->codec);
      int64_t start = d->fmt->start_time == AV_NOPTS_VALUE ? 0 : d->fmt->start_time;
      if (av_seek_frame(d->fmt, -1, start, AVSEEK_FLAG_BACKWARD) < 0) {
        set_error(error, error_size, "cannot restart the clip", 0);
        return -1;
      }
      continue;
    }
    if (ret < 0) { set_error(error, error_size, "cannot read the clip", ret); return -1; }
    if (p->packet->stream_index != d->stream_index) { av_packet_unref(p->packet); continue; }
    ret = avcodec_send_packet(d->codec, p->packet);
    av_packet_unref(p->packet);
    if (ret == AVERROR_INVALIDDATA && invalid < RAWFRAME_MAX_INVALID) { invalid++; continue; }
    if (ret < 0 && ret != AVERROR(EAGAIN)) { set_error(error, error_size, "decoder error", ret); return -1; }
    ret = avcodec_receive_frame(d->codec, d->frame);
    if (ret == 0) return 0;
    if (ret == AVERROR_INVALIDDATA && invalid < RAWFRAME_MAX_INVALID) { invalid++; continue; }
    if (ret != AVERROR(EAGAIN)) { set_error(error, error_size, "decoder error", ret); return -1; }
  }
}

int rawframe_player_next(RawFramePlayer *p, uint8_t *dst, size_t stride, char *error, size_t error_size) {
  if (!p || !dst || stride < (size_t)p->width * 4) { set_error(error, error_size, "bad arguments", 0); return -1; }
  if (!p->pending && player_decode_next(p, error, error_size) < 0) return -1;
  p->pending = 0;
  uint8_t *planes[4] = { dst, NULL, NULL, NULL };
  int strides[4] = { (int)stride, 0, 0, 0 };
  // Every frame is checked against the scaler (see scale_frame); the picture size stays fixed.
  return scale_frame(&p->sws, p->d.frame, p->width, p->height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR,
                     planes, strides, error, error_size);
}

void rawframe_player_close(RawFramePlayer *p) {
  if (!p) return;
  sws_freeContext(p->sws);
  av_packet_free(&p->packet);
  decoded_free(&p->d);
  free(p);
}

// ---- self test ------------------------------------------------------------------------------

static int make_frame(AVFrame *f, int w, int h, enum AVPixelFormat fmt, int value) {
  memset(f, 0, sizeof(*f));
  f->width = w;
  f->height = h;
  f->format = fmt;
  if (av_frame_get_buffer(f, 0) < 0) return -1;
  const int planes = av_pix_fmt_count_planes(fmt);
  for (int i = 0; i < planes; i++) {
    int lines = f->height;
    for (int y = 0; y < lines; y++) memset(f->data[i] + (size_t)y * f->linesize[i], value, (size_t)av_image_get_linesize(fmt, w, i));
  }
  return 0;
}

int rawframe_selftest_scale(char *error, size_t error_size) {
  enum { DW = 64, DH = 36, CANARY = 4096 };
  const size_t picture = (size_t)DW * 4 * DH;
  uint8_t *buffer = (uint8_t *)malloc(picture + CANARY);
  if (!buffer) { set_error(error, error_size, "out of memory", 0); return -1; }
  uint8_t *dst[4] = { buffer, NULL, NULL, NULL };
  int dst_stride[4] = { DW * 4, 0, 0, 0 };
  struct SwsContext *ctx = NULL;
  int result = -1;
  struct { int w, h; enum AVPixelFormat fmt; } sizes[] = {
    { 640, 360, AV_PIX_FMT_GBRP16 }, { 1000, 500, AV_PIX_FMT_GBRP16 }, { 300, 700, AV_PIX_FMT_GBRP },
    { 17, 9, AV_PIX_FMT_GBRP16 }, { 640, 360, AV_PIX_FMT_GBRP16 },
  };
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    AVFrame frame;
    if (make_frame(&frame, sizes[i].w, sizes[i].h, sizes[i].fmt, 0x80) < 0) {
      set_error(error, error_size, "selftest: cannot allocate a frame", 0);
      goto done;
    }
    memset(buffer, 0, picture);
    memset(buffer + picture, 0xA5, CANARY);
    int rc = scale_frame(&ctx, &frame, DW, DH, AV_PIX_FMT_BGR0, SWS_BICUBIC, dst, dst_stride, error, error_size);
    av_frame_unref(&frame);
    if (rc < 0) goto done;
    for (size_t k = 0; k < CANARY; k++) {
      if (buffer[picture + k] != 0xA5) {
        snprintf(error, error_size, "selftest: write past the picture after a %dx%d frame", sizes[i].w, sizes[i].h);
        goto done;
      }
    }
    // A constant source gives a constant picture: first and last pixel agree.
    const uint8_t *first = buffer, *last = buffer + picture - 4;
    for (int ch = 0; ch < 3; ch++) {
      if (abs((int)first[ch] - (int)last[ch]) > 2 || first[ch] == 0) {
        snprintf(error, error_size, "selftest: wrong picture after a %dx%d frame", sizes[i].w, sizes[i].h);
        goto done;
      }
    }
  }
  // Frames that must be refused, not scaled.
  AVFrame bad;
  memset(&bad, 0, sizeof(bad));
  bad.width = 0; bad.height = 100; bad.format = AV_PIX_FMT_GBRP16;
  if (scale_frame(&ctx, &bad, DW, DH, AV_PIX_FMT_BGR0, SWS_BICUBIC, dst, dst_stride, error, error_size) == 0) {
    set_error(error, error_size, "selftest: an empty frame was scaled", 0);
    goto done;
  }
  AVFrame fmtbad;
  if (make_frame(&fmtbad, 64, 64, AV_PIX_FMT_GBRP16, 0x40) < 0) goto done;
  fmtbad.format = AV_PIX_FMT_NONE;
  int refused = scale_frame(&ctx, &fmtbad, DW, DH, AV_PIX_FMT_BGR0, SWS_BICUBIC, dst, dst_stride, error, error_size);
  fmtbad.format = AV_PIX_FMT_GBRP16;
  av_frame_unref(&fmtbad);
  if (refused == 0) {
    set_error(error, error_size, "selftest: a frame without a pixel format was scaled", 0);
    goto done;
  }
  result = 0;
done:
  sws_freeContext(ctx);
  free(buffer);
  return result;
}
