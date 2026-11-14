//
//  rawframe.h
//  IINA RAW
//
//  Small C helper over libavformat, libavcodec and libswscale. It decodes one frame of a
//  Nikon raw clip (the tico_raw decoder) as the half-size Rec.709 view and converts it to a
//  BGR0 picture. Shared by the Quick Look extensions; it has no other dependency.
//

#ifndef RAWFRAME_H
#define RAWFRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Size of the error text buffers.
#define RAWFRAME_ERROR_SIZE 256

/// Facts about a clip. width and height are the size of the decoded half view.
typedef struct RawFrameInfo {
  int width;
  int height;
  double fps;               ///< 0 when unknown
  double duration_seconds;  ///< 0 when unknown
  char codec_long_name[128];
  int has_iso;
  int iso;
  int has_wb_gain;
  double wb_gain[3];
} RawFrameInfo;

/// A decoded picture: BGR0 (byte order B, G, R, unused), `stride` bytes per row.
/// Release it with rawframe_free_image.
typedef struct RawFrameImage {
  int width;
  int height;
  size_t stride;
  uint8_t *data;
} RawFrameImage;

/// Decode one frame at `fraction` (0...1) of the duration, scaled so that the long side is
/// at most `max_pixel_size` (the aspect ratio is kept; a smaller picture is not enlarged).
/// Returns 0 on success. On failure returns -1 and writes a message to `error`.
int rawframe_grab(const char *path, int max_pixel_size, double fraction,
                  RawFrameImage *out, char *error, size_t error_size);

/// Like rawframe_grab, and also fills `info` (when not NULL) with the facts of the decoded frame
/// and the clip (the size is that of the frame, fps and duration come from the stream).
int rawframe_grab_with_info(const char *path, int max_pixel_size, double fraction,
                            RawFrameImage *out, RawFrameInfo *info, char *error, size_t error_size);

/// Open the clip, decode one frame (the first frame) and fill `out`.
/// Returns 0 on success. On failure returns -1 and writes a message to `error`.
int rawframe_info(const char *path, RawFrameInfo *out, char *error, size_t error_size);

void rawframe_free_image(RawFrameImage *image);

/// Playback: a decoder that keeps one clip open and gives frame after frame, from the first
/// frame, and starts again at the first frame at the end of the clip (loop).
typedef struct RawFramePlayer RawFramePlayer;

/// Open the clip and decode its first frame. The pictures are scaled so that the long side is at
/// most `max_pixel_size` (never enlarged); `width` and `height` get their size, `info` (when
/// not NULL) the facts of the clip. Returns NULL on failure and writes a message to `error`.
RawFramePlayer *rawframe_player_open(const char *path, int max_pixel_size, int *width, int *height,
                                     RawFrameInfo *info, char *error, size_t error_size);

/// Decode the next frame and write it as BGRA (alpha 255) into `dst` (`stride` bytes per row, at least
/// width * 4; room for `height` rows). Returns 0 on success, -1 on failure (message in `error`).
int rawframe_player_next(RawFramePlayer *player, uint8_t *dst, size_t stride, char *error, size_t error_size);

void rawframe_player_close(RawFramePlayer *player);

/// Test of the scaling guard: scales synthetic frames of changing size and pixel format with one
/// scaler, checks that nothing is written past the picture, and that empty or format-less frames
/// are refused. Returns 0 when all is right, else -1 and a message in `error`.
int rawframe_selftest_scale(char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
