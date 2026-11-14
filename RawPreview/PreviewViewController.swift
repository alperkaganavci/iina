//
//  PreviewViewController.swift
//  IINA RAW
//
//  Quick Look preview extension (space bar in Finder) for Nikon raw clips (.R3D, .NEV).
//  Version 1: one frame from 10 % into the clip (long side 1920), black background, and one
//  info line under it. The frame comes from RawFrameGrabber (IINA RAW's own FFmpeg).
//  Version 2: after the still frame is up, the clip plays in the preview (half view, long side
//  1280, loop, at most the clip's frame rate) on an AVSampleBufferDisplayLayer. The playback
//  stops when the preview closes.
//

import AVFoundation
import Cocoa
import CoreMedia
import CoreVideo
import Quartz

/// State that the main queue sets and the playback queue reads (one per playback).
private final class StopFlag {
  private let lock = NSLock()
  private var stopped = false
  private var pausedFlag = false
  private var shown = 0
  var isStopped: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
  func stop() { lock.lock(); stopped = true; lock.unlock() }
  /// True while the preview window is not visible: the loop decodes nothing.
  var isPaused: Bool {
    get { lock.lock(); defer { lock.unlock() }; return pausedFlag }
    set { lock.lock(); pausedFlag = newValue; lock.unlock() }
  }
  /// Number of frames put on the display layer (for the test harness).
  var framesShown: Int { lock.lock(); defer { lock.unlock() }; return shown }
  func frameShown() { lock.lock(); shown += 1; lock.unlock() }
}

/// Number of playback loops that run now (for the test harness; it must never be more than 1).
private final class LoopCounter {
  private let lock = NSLock()
  private var count = 0
  var value: Int { lock.lock(); defer { lock.unlock() }; return count }
  func enter() { lock.lock(); count += 1; lock.unlock() }
  func leave() { lock.lock(); count -= 1; lock.unlock() }
}

/// A view whose layer shows video frames.
private final class VideoView: NSView {
  let displayLayer = AVSampleBufferDisplayLayer()

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
    layer = CALayer()
    layer?.backgroundColor = NSColor.black.cgColor
    displayLayer.videoGravity = .resizeAspect
    displayLayer.backgroundColor = NSColor.black.cgColor
    layer?.addSublayer(displayLayer)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func layout() {
    super.layout()
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    displayLayer.frame = bounds
    CATransaction.commit()
  }
}

class PreviewViewController: NSViewController, QLPreviewingController {

  private let imageView = NSImageView()
  private let videoView = VideoView(frame: .zero)
  private var stopFlag: StopFlag?
  private var lastFramesShown = 0
  /// Counts requests (and closes): a result of an older request is dropped. Main queue only.
  private var generation = 0
  /// Set by viewWillDisappear until the next request; no playback starts while it is set.
  private var closed = false
  private var occlusionObserver: NSObjectProtocol?
  private let loops = LoopCounter()
  private var currentURL: URL?
  private var droppedCompletions = 0
  private let playbackQueue = DispatchQueue(label: "com.alperkaganavci.iina-raw.preview.playback", qos: .userInitiated)
  private let infoLabel = NSTextField(labelWithString: "")
  private static let infoHeight: CGFloat = 32

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 960, height: 572))
    root.wantsLayer = true
    root.layer?.backgroundColor = NSColor.black.cgColor

    imageView.imageScaling = .scaleProportionallyUpOrDown
    imageView.imageAlignment = .alignCenter
    imageView.translatesAutoresizingMaskIntoConstraints = false

    infoLabel.textColor = NSColor(white: 0.85, alpha: 1)
    infoLabel.font = NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .regular)
    infoLabel.alignment = .center
    infoLabel.lineBreakMode = .byTruncatingTail
    infoLabel.translatesAutoresizingMaskIntoConstraints = false

    videoView.translatesAutoresizingMaskIntoConstraints = false
    videoView.isHidden = true

    root.addSubview(imageView)
    root.addSubview(videoView)
    root.addSubview(infoLabel)
    NSLayoutConstraint.activate([
      videoView.leadingAnchor.constraint(equalTo: imageView.leadingAnchor),
      videoView.trailingAnchor.constraint(equalTo: imageView.trailingAnchor),
      videoView.topAnchor.constraint(equalTo: imageView.topAnchor),
      videoView.bottomAnchor.constraint(equalTo: imageView.bottomAnchor),
      infoLabel.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 12),
      infoLabel.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -12),
      infoLabel.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -8),
      infoLabel.heightAnchor.constraint(equalToConstant: Self.infoHeight - 8),
      imageView.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      imageView.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      imageView.topAnchor.constraint(equalTo: root.topAnchor),
      imageView.bottomAnchor.constraint(equalTo: infoLabel.topAnchor, constant: -4),
    ])
    view = root
  }

  func preparePreviewOfFile(at url: URL, completionHandler handler: @escaping (Error?) -> Void) {
    // All state is touched on the main queue only.
    if Thread.isMainThread {
      beginPreview(url: url, handler: handler)
    } else {
      DispatchQueue.main.async { self.beginPreview(url: url, handler: handler) }
    }
  }

  private func beginPreview(url: URL, handler: @escaping (Error?) -> Void) {
    // A new request replaces everything of the old one: stop its playback now (a controller can
    // be reused for the next clip), show the still picture again, and make its late results stale.
    generation += 1
    let request = generation
    closed = false
    stopPlayback()
    imageView.image = nil
    infoLabel.stringValue = ""
    currentURL = url

    // Decode off the main thread; the handler may be called on any thread, after the view is set.
    DispatchQueue.global(qos: .userInitiated).async {
      let result: Result<(CGImage, RawClipInfo), Error>
      do { result = .success(try grabFrameAndInfo(url: url, maxPixelSize: 1920, at: 0.1)) } catch { result = .failure(error) }
      DispatchQueue.main.async {
        // A newer request, or a close, came while this one decoded: show and start nothing.
        guard request == self.generation, !self.closed else {
          self.droppedCompletions += 1
          handler(nil)
          return
        }
        switch result {
        case .failure(let error):
          handler(error)
        case .success(let (cgImage, info)):
          let size = NSSize(width: cgImage.width, height: cgImage.height)
          self.imageView.image = NSImage(cgImage: cgImage, size: size)
          self.infoLabel.stringValue = info.previewLine
          // Window size: the frame's aspect at 960 points wide, plus the info line.
          let width: CGFloat = 960
          self.preferredContentSize = NSSize(width: width, height: (width * size.height / size.width).rounded() + Self.infoHeight)
          handler(nil)
          self.startPlayback(url: url)
        }
      }
    }
  }

  // MARK: Playback (version 2)

  override func viewWillAppear() {
    super.viewWillAppear()
    closed = false
  }

  override func viewDidAppear() {
    super.viewDidAppear()
    observeOcclusion()
  }

  override func viewWillDisappear() {
    super.viewWillDisappear()
    // The preview closes: nothing that is still decoding may start a playback afterwards.
    generation += 1
    closed = true
    stopPlayback()
  }

  deinit {
    stopFlag?.stop()
    if let observer = occlusionObserver { NotificationCenter.default.removeObserver(observer) }
  }

  /// Frames shown by the current or the last playback, and the state of the display layer
  /// (test harness only).
  var playbackFramesShown: Int { stopFlag?.framesShown ?? lastFramesShown }
  var playbackLayerStatus: AVQueuedSampleBufferRenderingStatus { videoView.displayLayer.status }
  var playbackVideoVisible: Bool { !videoView.isHidden }
  var playbackImageVisible: Bool { !imageView.isHidden }
  /// Playback loops that run now.
  var playbackActiveLoops: Int { loops.value }
  /// The clip of the newest request.
  var playbackURL: URL? { currentURL }
  /// Results of older requests (or of a closed preview) that were dropped.
  var playbackDroppedCompletions: Int { droppedCompletions }
  var playbackPaused: Bool { stopFlag?.isPaused ?? false }

  /// Show the still picture again: no video frame stays on the layer.
  private func resetDisplay() {
    videoView.displayLayer.flushAndRemoveImage()
    videoView.isHidden = true
    imageView.isHidden = false
  }

  private func stopPlayback() {
    if let flag = stopFlag { lastFramesShown = flag.framesShown }
    stopFlag?.stop()
    stopFlag = nil
    resetDisplay()
  }

  /// Decode only while the preview window is visible. A window that was never shown (or is not
  /// on screen yet) does not pause the playback; a visible window that is fully covered does.
  private func observeOcclusion() {
    guard occlusionObserver == nil, let window = view.window else { return }
    occlusionObserver = NotificationCenter.default.addObserver(
      forName: NSWindow.didChangeOcclusionStateNotification, object: window, queue: .main) { [weak self] _ in
        self?.updatePaused(occluded: nil)
    }
    updatePaused(occluded: nil)
  }

  /// `occluded`: nil reads the window; a value is for the test harness.
  func updatePaused(occluded: Bool?) {
    let covered: Bool
    if let occluded = occluded {
      covered = occluded
    } else if let window = view.window, window.isVisible {
      covered = !window.occlusionState.contains(.visible)
    } else {
      covered = false
    }
    stopFlag?.isPaused = covered
  }

  /// Play the clip on a background queue: decode, put the frame in a CVPixelBuffer, enqueue it
  /// on the display layer, and wait for the clip's frame rate. If the decoder is slower than
  /// that, frames come as fast as they are decoded. Any failure leaves the still frame in place.
  private func startPlayback(url: URL) {
    stopPlayback()
    let flag = StopFlag()
    stopFlag = flag
    updatePaused(occluded: nil)
    let layer = videoView.displayLayer
    let loops = self.loops
    playbackQueue.async { [weak self] in
      loops.enter()
      defer {
        loops.leave()
        // However the loop ended, if it is still the current playback, show the still frame again.
        DispatchQueue.main.async {
          guard let self = self, self.stopFlag === flag else { return }
          self.stopPlayback()
        }
      }
      guard !flag.isStopped, let player = try? RawClipPlayer(url: url, maxPixelSize: 1280) else { return }
      let width = player.width, height = player.height
      let attributes: [CFString: Any] = [
        kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
        kCVPixelBufferWidthKey: width,
        kCVPixelBufferHeightKey: height,
        kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any],
      ]
      var pool: CVPixelBufferPool?
      guard CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &pool) == kCVReturnSuccess, let pool = pool else { return }
      // Frame rate 1...120 (a damaged file can say anything): the time stamps and the pacing
      // below cannot become zero or overflow.
      let rate = player.info.fps
      let fps = rate.isFinite && rate > 0 ? min(max(rate, 1), 120) : 25
      let frameDuration = 1.0 / fps
      let timescale: CMTimeScale = 90_000
      let tick = CMTimeValue((Double(timescale) / fps).rounded())
      var format: CMVideoFormatDescription?
      var shown = 0
      var start = DispatchTime.now().uptimeNanoseconds
      layer.flushAndRemoveImage()  // nothing of an earlier clip stays on the layer

      while !flag.isStopped {
        // The display layer failed: stop, and show the still frame again.
        if layer.status == .failed { return }
        // The window is not visible: decode nothing, and keep the pace afterwards.
        if flag.isPaused {
          usleep(50_000)
          start = DispatchTime.now().uptimeNanoseconds - UInt64(Double(shown) * frameDuration * 1_000_000_000)
          continue
        }
        // Wait until the layer wants a frame.
        if !layer.isReadyForMoreMediaData {
          usleep(2_000)
          continue
        }
        var buffer: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer) == kCVReturnSuccess, let pixelBuffer = buffer else { return }
        CVPixelBufferLockBaseAddress(pixelBuffer, [])
        let base = CVPixelBufferGetBaseAddress(pixelBuffer)!
        let stride = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let decoded: Bool
        do { try player.nextFrame(into: base, bytesPerRow: stride); decoded = true } catch { decoded = false }
        CVPixelBufferUnlockBaseAddress(pixelBuffer, [])
        if !decoded || flag.isStopped { return }

        if format == nil {
          CMVideoFormatDescriptionCreateForImageBuffer(allocator: nil, imageBuffer: pixelBuffer, formatDescriptionOut: &format)
        }
        guard let description = format else { return }
        var timing = CMSampleTimingInfo(duration: CMTime(value: tick, timescale: timescale),
                                        presentationTimeStamp: CMTime(value: CMTimeValue(shown) * tick, timescale: timescale),
                                        decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        CMSampleBufferCreateReadyWithImageBuffer(allocator: nil, imageBuffer: pixelBuffer, formatDescription: description,
                                                 sampleTiming: &timing, sampleBufferOut: &sample)
        guard let sampleBuffer = sample else { return }
        // Show the frame when it is enqueued; this queue sets the pace.
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: true) as? [NSMutableDictionary],
           let first = attachments.first {
          first[kCMSampleAttachmentKey_DisplayImmediately] = true
        }
        layer.enqueue(sampleBuffer)
        flag.frameShown()
        if shown == 0 {
          DispatchQueue.main.async {
            guard let self = self, self.stopFlag === flag else { return }
            self.videoView.isHidden = false
            self.imageView.isHidden = true
          }
        }
        shown += 1

        // Pace: frame n is due at start + n * frameDuration (at most 1 s per frame). Never skip frames.
        let due = start &+ UInt64(Double(shown) * frameDuration * 1_000_000_000)
        let now = DispatchTime.now().uptimeNanoseconds
        if due > now { usleep(UInt32(min((due - now) / 1_000, 1_000_000))) }
      }
    }
  }
}
