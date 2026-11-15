// Draws the launcher icon (glass towers, one red, a runner-vision line) into an .iconset folder: swift make-icon.swift <dir>
import AppKit

func draw(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px), ctx = NSGraphicsContext.current!.cgContext
    let cs = CGColorSpaceCreateDeviceRGB()
    let inset = s * 0.055, body = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let clip = NSBezierPath(roundedRect: body, xRadius: s * 0.225, yRadius: s * 0.225)
    ctx.saveGState(); clip.addClip()
    // sky
    let sky = CGGradient(colorsSpace: cs, colors: [CGColor(red: 0.18, green: 0.45, blue: 0.78, alpha: 1), CGColor(red: 0.82, green: 0.92, blue: 0.99, alpha: 1)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(sky, start: CGPoint(x: 0, y: s), end: CGPoint(x: 0, y: s * 0.3), options: [])
    // towers: x, width, height (fractions), red?
    let towers: [(CGFloat, CGFloat, CGFloat, Bool)] = [
        (0.04, 0.16, 0.46, false), (0.18, 0.12, 0.64, false), (0.29, 0.19, 0.92, false),
        (0.47, 0.15, 0.58, true), (0.61, 0.17, 0.80, false), (0.77, 0.19, 0.50, false)]
    for (i, t) in towers.enumerated() {
        let r = CGRect(x: t.0 * s, y: 0, width: t.1 * s, height: t.2 * s)
        let cols: [CGColor] = t.3
            ? [CGColor(red: 1, green: 0.32, blue: 0.30, alpha: 1), CGColor(red: 0.78, green: 0.05, blue: 0.10, alpha: 1)]
            : [CGColor(red: 0.93, green: 0.97, blue: 1, alpha: 1), CGColor(red: 0.45, green: 0.66, blue: 0.86, alpha: 1)]
        let g = CGGradient(colorsSpace: cs, colors: cols as CFArray, locations: [0, 1])!
        ctx.saveGState(); ctx.clip(to: r)
        ctx.drawLinearGradient(g, start: CGPoint(x: r.minX, y: r.maxY), end: CGPoint(x: r.maxX, y: r.minY), options: [])
        // glass reflection streak and floor lines
        ctx.setFillColor(CGColor(gray: 1, alpha: 0.35))
        ctx.move(to: CGPoint(x: r.minX, y: r.maxY)); ctx.addLine(to: CGPoint(x: r.minX + r.width * 0.45, y: r.maxY))
        ctx.addLine(to: CGPoint(x: r.minX + r.width * 0.15, y: r.minY)); ctx.addLine(to: CGPoint(x: r.minX, y: r.minY)); ctx.fillPath()
        ctx.setStrokeColor(CGColor(gray: 1, alpha: 0.28)); ctx.setLineWidth(max(1, s * 0.004))
        var y = r.maxY - s * 0.045
        while y > 0 { ctx.move(to: CGPoint(x: r.minX, y: y)); ctx.addLine(to: CGPoint(x: r.maxX, y: y)); ctx.strokePath(); y -= s * 0.045 }
        ctx.restoreGState()
        ctx.setStrokeColor(CGColor(red: 0.10, green: 0.25, blue: 0.45, alpha: 0.55)); ctx.setLineWidth(s * 0.006)
        ctx.stroke(r)
        _ = i
    }
    // runner-vision line
    ctx.setStrokeColor(CGColor(red: 0.93, green: 0.11, blue: 0.14, alpha: 1)); ctx.setLineWidth(s * 0.022); ctx.setLineCap(.round)
    ctx.move(to: CGPoint(x: s * 0.10, y: s * 0.12)); ctx.addLine(to: CGPoint(x: s * 0.57, y: s * 0.60)); ctx.addLine(to: CGPoint(x: s * 0.90, y: s * 0.80)); ctx.strokePath()
    // glass sheen over the top
    let sheen = CGGradient(colorsSpace: cs, colors: [CGColor(gray: 1, alpha: 0.45), CGColor(gray: 1, alpha: 0)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(sheen, start: CGPoint(x: 0, y: s), end: CGPoint(x: 0, y: s * 0.62), options: [])
    ctx.restoreGState()
    // rim
    NSColor(white: 1, alpha: 0.6).setStroke(); clip.lineWidth = max(1, s * 0.006); clip.stroke()
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let dir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.iconset"
try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    try! draw(base).write(to: URL(fileURLWithPath: "\(dir)/icon_\(base)x\(base).png"))
    try! draw(base * 2).write(to: URL(fileURLWithPath: "\(dir)/icon_\(base)x\(base)@2x.png"))
}
