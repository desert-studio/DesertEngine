import CoreImage
import AppKit
let a = CommandLine.arguments
let img = CIImage(contentsOf: URL(fileURLWithPath: a[1]))!
let u0 = CGFloat(Double(a[3])!), u1 = CGFloat(Double(a[4])!), horizonFromTop = CGFloat(Double(a[5])!), ev = Float(a[6])!
let W = img.extent.width, H = img.extent.height
let cw = (u1 - u0) * W, ch = cw * 9 / 16
let top = 0.493 * H - horizonFromTop * ch
let rect = CGRect(x: u0 * W, y: H - (top + ch), width: cw, height: ch)
var e = img.cropped(to: rect).applyingFilter("CIExposureAdjust", parameters: ["inputEV": ev])
// Reinhard on luminance-ish per channel: x/(1+x), then gamma handled by sRGB output
let k = CIColorKernel(source: "kernel vec4 r(__sample s){ vec3 c = s.rgb; vec3 m = c*(1.0+c/16.0)/(1.0+c); return vec4(m, 1.0); }")!
e = k.apply(extent: e.extent, arguments: [e])!
e = e.applyingFilter("CIColorControls", parameters: ["inputContrast": 1.18, "inputSaturation": 1.12, "inputBrightness": -0.02])
e = e.applyingFilter("CITemperatureAndTint", parameters: ["inputNeutral": CIVector(x: 6500, y: 0), "inputTargetNeutral": CIVector(x: 5600, y: 0)])
e = e.applyingFilter("CIVignette", parameters: ["inputIntensity": 0.55, "inputRadius": 1.6])
let ctx = CIContext(options: [.workingColorSpace: CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!])
let cg = ctx.createCGImage(e, from: e.extent, format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)!
try! NSBitmapImageRep(cgImage: cg).representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[2]))
print(Int(cw), Int(ch))
