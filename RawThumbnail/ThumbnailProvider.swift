//
//  ThumbnailProvider.swift
//  IINA RAW
//
//  Quick Look thumbnail extension: Finder shows a real frame of a Nikon raw clip (.R3D, .NEV).
//  The frame comes from RawFrameGrabber (IINA RAW's own FFmpeg with the tico_raw decoder).
//

import AppKit
import QuickLookThumbnailing

class ThumbnailProvider: QLThumbnailProvider {

  override func provideThumbnail(for request: QLFileThumbnailRequest,
                                 _ handler: @escaping (QLThumbnailReply?, Error?) -> Void) {
    let maximum = request.maximumSize
    // A size that is not finite (or absurd) must not trap the conversion to Int.
    let wanted = max(maximum.width, maximum.height) * request.scale
    let longSidePixels = wanted.isFinite ? Int(min(max(wanted.rounded(), 1), 16384)) : 512
    let image: CGImage
    do {
      // 10 % into the clip: the first frames of a clip are often slates or black.
      image = try grabFrame(url: request.fileURL, maxPixelSize: longSidePixels, at: 0.1)
    } catch {
      handler(nil, error)
      return
    }

    // Aspect-fit the picture into the requested size (in points).
    let aspect = CGFloat(image.width) / CGFloat(image.height)
    var width = maximum.width
    var height = width / aspect
    if height > maximum.height {
      height = maximum.height
      width = height * aspect
    }
    let contextSize = CGSize(width: max(1, width.rounded()), height: max(1, height.rounded()))

    let reply = QLThumbnailReply(contextSize: contextSize, currentContextDrawing: { () -> Bool in
      guard let context = NSGraphicsContext.current?.cgContext else { return false }
      context.interpolationQuality = .high
      context.draw(image, in: CGRect(origin: .zero, size: contextSize))
      return true
    })
    handler(reply, nil)
  }
}
