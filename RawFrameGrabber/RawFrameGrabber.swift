//
//  RawFrameGrabber.swift
//  IINA RAW
//
//  Frame grabber for Nikon raw clips (.R3D, .NEV), used by the Quick Look extensions.
//  It decodes one frame with IINA RAW's own FFmpeg (the tico_raw decoder, half-size Rec.709
//  view) through the C helper in rawframe.c.
//

import CoreGraphics
import Foundation

struct RawClipInfo {
  /// Size of the decoded half view.
  let width: Int
  let height: Int
  let fps: Double
  let durationSeconds: Double
  let codecLongName: String
  /// Nil when the frame carries no `ticoraw.iso` metadata.
  let iso: Int?
  /// Nil when the frame carries no `ticoraw.wb_gain` metadata (red, green, blue gain).
  let whiteBalance: [Double]?
}

enum RawFrameGrabberError: Error, LocalizedError {
  case failed(String)

  var errorDescription: String? {
    switch self {
    case .failed(let message): return "IINA RAW frame grabber: \(message)"
    }
  }
}

private func errorText(_ buffer: [CChar]) -> String {
  return String(cString: buffer)
}

/// Wrap a decoded BGR0 picture in a CGImage (copies the pixels) and release the C buffer.
private func makeImage(_ image: inout RawFrameImage) throws -> CGImage {
  defer { rawframe_free_image(&image) }
  guard let data = image.data else { throw RawFrameGrabberError.failed("no picture data") }
  let bytes = image.stride * Int(image.height)
  guard let provider = CGDataProvider(data: Data(bytes: data, count: bytes) as CFData) else {
    throw RawFrameGrabberError.failed("cannot create the image data")
  }
  // BT.709 where the system has it; sRGB otherwise.
  let space = CGColorSpace(name: CGColorSpace.itur_709) ?? CGColorSpace(name: CGColorSpace.sRGB)!
  // BGR0: blue, green, red, unused byte = 32-bit little endian, alpha skipped (first).
  let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
  guard let cg = CGImage(width: Int(image.width), height: Int(image.height),
                         bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: image.stride,
                         space: space, bitmapInfo: info, provider: provider, decode: nil,
                         shouldInterpolate: true, intent: .defaultIntent) else {
    throw RawFrameGrabberError.failed("cannot create the image")
  }
  return cg
}

private func clipInfo(from info: RawFrameInfo) -> RawClipInfo {
  var nameField = info.codec_long_name
  let codec = withUnsafePointer(to: &nameField) {
    $0.withMemoryRebound(to: CChar.self, capacity: 128) { String(cString: $0) }
  }
  let wb: [Double]? = info.has_wb_gain != 0 ? [info.wb_gain.0, info.wb_gain.1, info.wb_gain.2] : nil
  return RawClipInfo(width: Int(info.width), height: Int(info.height), fps: info.fps,
                     durationSeconds: info.duration_seconds, codecLongName: codec,
                     iso: info.has_iso != 0 ? Int(info.iso) : nil, whiteBalance: wb)
}

/// Decode one frame at `fraction` (0...1) of the clip and return it as a CGImage whose long
/// side is at most `maxPixelSize`. The decoder runs with `view=rec709`, `view_size=half`,
/// `thread_type=slice` and four threads. When the seek fails, the first frame is used.
func grabFrame(url: URL, maxPixelSize: Int, at fraction: Double = 0.1) throws -> CGImage {
  return try grabFrameAndInfo(url: url, maxPixelSize: maxPixelSize, at: fraction).image
}

/// Like `grabFrame`, and also the facts of the clip, from the same single decode.
func grabFrameAndInfo(url: URL, maxPixelSize: Int, at fraction: Double = 0.1) throws -> (image: CGImage, info: RawClipInfo) {
  var image = RawFrameImage()
  var info = RawFrameInfo()
  var error = [CChar](repeating: 0, count: Int(RAWFRAME_ERROR_SIZE))
  let status = url.path.withCString {
    rawframe_grab_with_info($0, Int32(min(max(maxPixelSize, 1), 16384)), fraction, &image, &info, &error, error.count)
  }
  guard status == 0 else { throw RawFrameGrabberError.failed(errorText(error)) }
  return (try makeImage(&image), clipInfo(from: info))
}

/// Facts about the clip: size of the half view, frame rate, duration, codec name, and the
/// ISO and white balance of the first frame when the decoder gives them.
func clipInfo(url: URL) throws -> RawClipInfo {
  var info = RawFrameInfo()
  var error = [CChar](repeating: 0, count: Int(RAWFRAME_ERROR_SIZE))
  let status = url.path.withCString { rawframe_info($0, &info, &error, error.count) }
  guard status == 0 else {
    throw RawFrameGrabberError.failed(errorText(error))
  }
  return clipInfo(from: info)
}

extension RawClipInfo {
  /// `25`, `23.976`: at most three decimals, no trailing zeros. Nil for a frame rate that is
  /// not finite, below 0.001 (it would print as "0") or above 1000 (a damaged file).
  static func formatFps(_ fps: Double) -> String? {
    guard fps.isFinite, fps >= 0.001, fps <= 1000 else { return nil }
    var text = String(format: "%.3f", fps)
    while text.hasSuffix("0") { text.removeLast() }
    if text.hasSuffix(".") { text.removeLast() }
    return text
  }

  /// `mm:ss`, rounded to the second. Nil for a duration that is not finite, not positive or
  /// 24 hours or more (a damaged file); nothing here can trap.
  static func formatDuration(_ seconds: Double) -> String? {
    guard seconds.isFinite, seconds > 0, seconds < 86_400, let total = Int(exactly: seconds.rounded()) else { return nil }
    return String(format: "%02d:%02d", total / 60, total % 60)
  }

  /// The fields of the preview's info line, in order: `3030×1705`, `25 fps`, `00:33`, `ISO 6400`,
  /// `TICO RAW`. Fields that are missing, or damaged, are left out. The preview drops fields
  /// from the end when the line does not fit its window.
  var previewFields: [String] {
    var parts: [String] = []
    if width > 0 && height > 0 { parts.append("\(width)\u{00D7}\(height)") }
    if let text = RawClipInfo.formatFps(fps) { parts.append("\(text) fps") }
    if let text = RawClipInfo.formatDuration(durationSeconds) { parts.append(text) }
    if let iso = iso, iso >= 0 { parts.append("ISO \(iso)") }
    parts.append("TICO RAW")
    return parts
  }

  /// One line for the Quick Look preview: `3030×1705 · 25 fps · 00:33 · ISO 6400 · TICO RAW`.
  var previewLine: String { previewFields.joined(separator: " \u{00B7} ") }
}

/// Playback of a clip for the Quick Look preview: frame after frame at the half view, scaled
/// to BGRA, and back to the first frame at the end (loop). Use it from one queue at a time.
final class RawClipPlayer {
  /// Size of the pictures `nextFrame` writes (BGRA, opaque).
  let width: Int
  let height: Int
  let info: RawClipInfo
  private var handle: OpaquePointer?

  init(url: URL, maxPixelSize: Int) throws {
    var w: Int32 = 0
    var h: Int32 = 0
    var raw = RawFrameInfo()
    var error = [CChar](repeating: 0, count: Int(RAWFRAME_ERROR_SIZE))
    let limit = Int32(min(max(maxPixelSize, 1), 16384))
    let player = url.path.withCString { rawframe_player_open($0, limit, &w, &h, &raw, &error, error.count) }
    guard let player = player else { throw RawFrameGrabberError.failed(errorText(error)) }
    handle = player
    width = Int(w)
    height = Int(h)
    info = clipInfo(from: raw)
  }

  deinit {
    rawframe_player_close(handle)
  }

  /// Decode the next frame into `base` (BGRA, `bytesPerRow` bytes per row, `height` rows).
  func nextFrame(into base: UnsafeMutableRawPointer, bytesPerRow: Int) throws {
    var error = [CChar](repeating: 0, count: Int(RAWFRAME_ERROR_SIZE))
    let status = rawframe_player_next(handle, base.assumingMemoryBound(to: UInt8.self), bytesPerRow, &error, error.count)
    if status != 0 { throw RawFrameGrabberError.failed(errorText(error)) }
  }
}

/// Test of the scaling guard in rawframe.c (see `rawframe_selftest_scale`).
func rawFrameSelfTestScale() throws {
  var error = [CChar](repeating: 0, count: Int(RAWFRAME_ERROR_SIZE))
  if rawframe_selftest_scale(&error, error.count) != 0 { throw RawFrameGrabberError.failed(errorText(error)) }
}
