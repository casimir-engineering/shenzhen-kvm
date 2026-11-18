// Renders the Shenzhen KVM icon at any square pixel size, in the ShenzhenPDF / ShenzhenQR family: an
// edge-to-edge light backdrop that deepens to one colour in the lower right, a saturated blue object
// with a cast shadow and an offset rear layer, and the white 深圳 signature. Here the backdrop is violet
// and the object is two screens side by side with one pointer crossing the gap between them.
//
//   swift make-icon.swift <out.png> <pixel-size> [variant]
//
// variant: "twin" (default, the shipped icon), "keyboard" (two screens over one shared keyboard bar),
// "stack" (one screen behind the other, pointer leaving the front one): earlier drafts.
// mac/icon/AppIcon.icns is made from it with iconutil (sizes 16-1024); linux/files/icons from the same.
import AppKit

let args = CommandLine.arguments
guard args.count >= 3, let pixelSize = Int(args[2]), pixelSize > 0 else {
    FileHandle.standardError.write("usage: make-icon.swift <out.png> <pixel-size> [twin|keyboard|stack]\n".data(using: .utf8)!)
    exit(2)
}
let variant = args.count > 3 ? args[3] : "twin"

let outputURL = URL(fileURLWithPath: args[1])
let size = CGFloat(pixelSize)
let scale = size / 1024.0

func point(_ x: CGFloat, _ y: CGFloat) -> NSPoint { NSPoint(x: x * scale, y: y * scale) }
func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> NSRect {
    NSRect(x: x * scale, y: y * scale, width: w * scale, height: h * scale)
}
func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1.0) -> NSColor {
    NSColor(calibratedRed: r / 255.0, green: g / 255.0, blue: b / 255.0, alpha: a)
}
func rounded(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ r: CGFloat) -> NSBezierPath {
    NSBezierPath(roundedRect: rect(x, y, w, h), xRadius: r * scale, yRadius: r * scale)
}

// A blue object exactly as the siblings draw it: soft cast shadow, darker rear layer offset
// down-right, front face with the vertical blue gradient and a pale top highlight.
func drawObject(_ shape: (CGFloat, CGFloat) -> NSBezierPath, highlight: (CGFloat, CGFloat, CGFloat)?) {
    let castShadow = NSShadow()
    castShadow.shadowColor = color(40, 20, 120, 0.32)
    castShadow.shadowBlurRadius = 54 * scale
    castShadow.shadowOffset = NSSize(width: 0, height: -25 * scale)
    NSGraphicsContext.current?.saveGraphicsState()
    castShadow.set()
    color(0, 74, 196).setFill()
    shape(18, -18).fill()
    NSGraphicsContext.current?.restoreGraphicsState()

    NSGradient(starting: color(0, 73, 190), ending: color(0, 112, 236))!.draw(in: shape(18, -18), angle: 92)
    NSGradient(colorsAndLocations:
        (color(24, 151, 255), 0.0),
        (color(4, 111, 244), 0.50),
        (color(0, 79, 215), 1.0))!.draw(in: shape(0, 0), angle: 104)

    if let (x0, x1, y) = highlight {
        let line = NSBezierPath()
        line.move(to: point(x0, y))
        line.line(to: point(x1, y))
        line.lineWidth = max(1, 5 * scale)
        line.lineCapStyle = .round
        color(144, 216, 255, 0.72).setStroke()
        line.stroke()
    }
}

func drawText(_ text: String, in box: NSRect, font: NSFont, tracking: CGFloat = 0) {
    let paragraph = NSMutableParagraphStyle()
    paragraph.alignment = .center
    let shadow = NSShadow()
    shadow.shadowColor = color(0, 48, 142, 0.22)
    shadow.shadowBlurRadius = 7 * scale
    shadow.shadowOffset = NSSize(width: 0, height: -3 * scale)
    NSAttributedString(string: text, attributes: [
        .font: font, .foregroundColor: NSColor.white, .paragraphStyle: paragraph,
        .kern: tracking * scale, .shadow: shadow,
    ]).draw(in: box)
}

// The macOS pointer arrow (dark mode: black with a white outline), tip at (tipX, tipY), `unit` = canvas units per arrow unit.
func drawPointer(tipX: CGFloat, tipY: CGFloat, unit: CGFloat) {
    let outline: [(CGFloat, CGFloat)] = [(0, 0), (0, 17), (4.1, 13.1), (6.9, 19.6), (9.6, 18.4), (6.9, 12.1), (12.2, 12.1)]
    let path = NSBezierPath()
    for (i, (ax, ay)) in outline.enumerated() {
        let p = point(tipX + ax * unit, tipY - ay * unit)
        if i == 0 { path.move(to: p) } else { path.line(to: p) }
    }
    path.close()
    path.lineJoinStyle = .round
    let shadow = NSShadow()
    shadow.shadowColor = color(20, 10, 80, 0.40)
    shadow.shadowBlurRadius = 16 * scale
    shadow.shadowOffset = NSSize(width: 0, height: -8 * scale)
    NSGraphicsContext.current?.saveGraphicsState()
    shadow.set()
    // Dark-mode macOS pointer: black fill, thin white outline.
    NSColor.white.setStroke()
    path.lineWidth = max(1.2, 1.8 * unit * scale)
    path.stroke()
    NSGraphicsContext.current?.restoreGraphicsState()
    NSColor.black.setFill()
    path.fill()
}

guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixelSize, pixelsHigh: pixelSize,
                                    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else {
    fatalError("failed to create bitmap")
}
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
NSGraphicsContext.current?.imageInterpolation = .high
NSGraphicsContext.current?.shouldAntialias = true

let canvas = rect(0, 0, 1024, 1024)
NSColor.clear.setFill()
canvas.fill()

// Backdrop: same stops and angle as the siblings, violet where PDF is orange, QR coral, Files blue.
NSGradient(colorsAndLocations:
    (color(253, 251, 255), 0.0),
    (color(228, 210, 255), 0.46),
    (color(126, 62, 226), 1.0))!.draw(in: canvas, angle: -38)
NSGradient(colorsAndLocations:
    (color(255, 255, 255, 0.72), 0.0),
    (color(255, 255, 255, 0.0), 1.0))!
    .draw(in: rect(-160, 490, 760, 700), relativeCenterPosition: NSPoint(x: -0.25, y: 0.25))

let hanzi = { (pt: CGFloat) in NSFont(name: "PingFangSC-Semibold", size: pt * scale) ?? NSFont.systemFont(ofSize: pt * scale, weight: .bold) }
let label = NSFont.systemFont(ofSize: 126 * scale, weight: .heavy)

// Two screens: landscape panels with a small stand, side by side with a gap.
func screen(_ x: CGFloat) -> (CGFloat, CGFloat) -> NSBezierPath {
    return { dx, dy in
        let p = rounded(x + dx, 400 + dy, 382, 326, 40)
        p.append(rounded(x + 161 + dx, 328 + dy, 60, 90, 8))     // neck
        p.append(rounded(x + 101 + dx, 300 + dy, 180, 40, 20))   // foot
        return p
    }
}

switch variant {
case "keyboard":
    let left = { (dx: CGFloat, dy: CGFloat) in rounded(118 + dx, 450 + dy, 372, 300, 38) }
    let right = { (dx: CGFloat, dy: CGFloat) in rounded(534 + dx, 450 + dy, 372, 300, 38) }
    let bar = { (dx: CGFloat, dy: CGFloat) in rounded(210 + dx, 196 + dy, 604, 176, 44) }
    drawObject(left, highlight: (178, 430, 750))
    drawObject(right, highlight: (594, 846, 750))
    drawObject(bar, highlight: (262, 762, 372))
    drawText("深", in: rect(118, 466, 372, 260), font: hanzi(200))
    drawText("圳", in: rect(534, 466, 372, 260), font: hanzi(200))
    drawText("KVM", in: rect(210, 184, 604, 170), font: label, tracking: 8)
    drawPointer(tipX: 486, tipY: 640, unit: 9.5)
case "stack":
    let back = { (dx: CGFloat, dy: CGFloat) in rounded(380 + dx, 420 + dy, 500, 380, 44) }
    let front = { (dx: CGFloat, dy: CGFloat) in rounded(144 + dx, 216 + dy, 560, 440, 48) }
    drawObject(back, highlight: (440, 820, 800))
    drawObject(front, highlight: (204, 644, 656))
    drawText("深圳", in: rect(144, 400, 560, 230), font: hanzi(176))
    drawText("KVM", in: rect(144, 216, 560, 180), font: label, tracking: 6)
    drawPointer(tipX: 694, tipY: 700, unit: 10)
default: // twin
    drawObject(screen(98), highlight: (154, 424, 726))
    drawObject(screen(526), highlight: (582, 852, 726))
    drawText("深", in: rect(98, 438, 382, 270), font: hanzi(210))
    drawText("圳", in: rect(526, 438, 382, 270), font: hanzi(210))
    drawPointer(tipX: 474, tipY: 640, unit: 11.5)
}

NSGraphicsContext.restoreGraphicsState()
guard let png = bitmap.representation(using: .png, properties: [:]) else {
    FileHandle.standardError.write("failed to encode PNG\n".data(using: .utf8)!)
    exit(1)
}
try png.write(to: outputURL)
