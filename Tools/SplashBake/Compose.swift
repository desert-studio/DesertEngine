import AppKit
let a = CommandLine.arguments
let bg = NSImage(contentsOfFile: a[1])!, out = a[2], cropY = Double(a[3])!, variant = a[4]
let W: CGFloat = 1200, H: CGFloat = 675
let img = NSImage(size: NSSize(width: W, height: H))
img.lockFocus()
// background: cover-crop, vertical offset cropY (0..1)
let s = max(W / bg.size.width, H / bg.size.height)
let dw = bg.size.width * s, dh = bg.size.height * s
bg.draw(in: NSRect(x: (W - dw) / 2, y: -(dh - H) * CGFloat(cropY), width: dw, height: dh), from: .zero, operation: .copy, fraction: 1)
// exposure lift + bottom gradient for legibility
NSColor(white: 1, alpha: 0.06).setFill(); NSRect(x: 0, y: 0, width: W, height: H).fill(using: .plusLighter)
NSGradient(starting: NSColor(white: 0, alpha: 0.78), ending: NSColor(white: 0, alpha: 0))!.draw(in: NSRect(x: 0, y: 0, width: W, height: 260), angle: 90)
func text(_ s: String, _ x: CGFloat, _ y: CGFloat, _ f: NSFont, _ c: NSColor, kern: CGFloat = 0, glow: NSColor? = nil, radius: CGFloat = 0) {
  var attrs: [NSAttributedString.Key: Any] = [.font: f, .foregroundColor: c, .kern: kern]
  if let g = glow { let sh = NSShadow(); sh.shadowColor = g; sh.shadowBlurRadius = radius; sh.shadowOffset = .zero; attrs[.shadow] = sh }
  (s as NSString).draw(at: NSPoint(x: x, y: y), withAttributes: attrs)
}
let white = NSColor.white, dim = NSColor(white: 1, alpha: 0.62), sand = NSColor(calibratedRed: 0.95, green: 0.74, blue: 0.45, alpha: 1)
// wordmark
text("DESERT", 48, 118, NSFont.systemFont(ofSize: 58, weight: .heavy), white, kern: 10, glow: NSColor(calibratedRed: 1.0, green: 0.72, blue: 0.38, alpha: 0.85), radius: 22)
text("DESERT", 48, 118, NSFont.systemFont(ofSize: 58, weight: .heavy), white, kern: 10, glow: NSColor(white:1, alpha:0.55), radius: 6)
text("ENGINE", 52, 94, NSFont.systemFont(ofSize: 20, weight: .medium), sand, kern: 14.5, glow: NSColor(calibratedRed: 1.0, green: 0.72, blue: 0.38, alpha: 0.85), radius: 12)
text("© 2026 Desert Studio", 48, 4, NSFont.systemFont(ofSize: 9.5, weight: .regular), NSColor(white: 1, alpha: 0.38))
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .jpeg, properties: [.compressionFactor: 0.95])!.write(to: URL(fileURLWithPath: out))
