// Renders the AES67eleven logo (two led meter bars forming "11" over a sine
// wave) as the app icon master png and the menu bar template icon.
// usage: swift render_icons.swift <outdir>
import AppKit

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."

func savePng(_ image: NSImage, _ path: String, size: Int) {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    image.draw(in: NSRect(x: 0, y: 0, width: size, height: size))
    NSGraphicsContext.restoreGraphicsState()
    try! rep.representation(using: .png, properties: [:])!
        .write(to: URL(fileURLWithPath: path))
}

func wavePath(_ s: CGFloat) -> NSBezierPath {
    // gentle sine across the lower third (flipped coords: y grows downward here we draw in normal cartesian)
    let path = NSBezierPath()
    let y0 = 0.25 * s
    path.move(to: NSPoint(x: 0.14*s, y: y0))
    path.curve(to: NSPoint(x: 0.36*s, y: y0),
               controlPoint1: NSPoint(x: 0.22*s, y: y0 + 0.09*s),
               controlPoint2: NSPoint(x: 0.28*s, y: y0 + 0.09*s))
    path.curve(to: NSPoint(x: 0.58*s, y: y0),
               controlPoint1: NSPoint(x: 0.44*s, y: y0 - 0.09*s),
               controlPoint2: NSPoint(x: 0.50*s, y: y0 - 0.09*s))
    path.curve(to: NSPoint(x: 0.86*s, y: y0),
               controlPoint1: NSPoint(x: 0.66*s, y: y0 + 0.09*s),
               controlPoint2: NSPoint(x: 0.74*s, y: y0 + 0.09*s))
    return path
}

func drawBars(_ s: CGFloat, color: NSColor?, ledGaps: Bool) {
    // two rounded meter bars forming "11" (right one taller)
    let barW = 0.125 * s
    let bars = [NSRect(x: 0.33*s, y: 0.34*s, width: barW, height: 0.44*s),
                NSRect(x: 0.55*s, y: 0.34*s, width: barW, height: 0.53*s)]
    for (i, bar) in bars.enumerated() {
        if let mono = color {
            mono.setFill()
            NSBezierPath(roundedRect: bar, xRadius: barW*0.38, yRadius: barW*0.38).fill()
        } else {
            let gradient = NSGradient(colorsAndLocations:
                (NSColor(calibratedRed: 0.11, green: 0.62, blue: 0.34, alpha: 1), 0.0),
                (NSColor(calibratedRed: 0.18, green: 0.80, blue: 0.44, alpha: 1), 0.70),
                (NSColor(calibratedRed: 0.95, green: 0.77, blue: 0.06, alpha: 1), 0.80),
                (NSColor(calibratedRed: 0.95, green: 0.77, blue: 0.06, alpha: 1), 1.0))!
            gradient.draw(in: NSBezierPath(roundedRect: bar, xRadius: barW*0.38, yRadius: barW*0.38), angle: 90)
        }
        if ledGaps {
            NSColor(calibratedRed: 0.05, green: 0.07, blue: 0.11, alpha: 0.92).setFill()
            let nGaps = i == 0 ? 3 : 4
            for g in 1...nGaps {
                let y = bar.minY + bar.height * CGFloat(g) / CGFloat(nGaps + 1)
                NSRect(x: bar.minX - 0.01*s, y: y, width: bar.width + 0.02*s, height: 0.028*s).fill()
            }
        }
    }
}

// ---- app icon (1024) ----
let appIcon = NSImage(size: NSSize(width: 1024, height: 1024), flipped: false) { _ in
    let s: CGFloat = 1024
    // macos style: margin around the squircle
    let badge = NSRect(x: 0.06*s, y: 0.06*s, width: 0.88*s, height: 0.88*s)
    let squircle = NSBezierPath(roundedRect: badge, xRadius: 0.20*s, yRadius: 0.20*s)
    NSGradient(colorsAndLocations:
        (NSColor(calibratedRed: 0.05, green: 0.07, blue: 0.11, alpha: 1), 0.0),
        (NSColor(calibratedRed: 0.11, green: 0.15, blue: 0.20, alpha: 1), 1.0))!
        .draw(in: squircle, angle: 90)
    let wave = wavePath(s)
    wave.lineWidth = 0.032 * s
    wave.lineCapStyle = .round
    NSColor(calibratedRed: 0.30, green: 0.64, blue: 1.0, alpha: 0.85).setStroke()
    wave.stroke()
    drawBars(s, color: nil, ledGaps: true)
    return true
}
savePng(appIcon, "\(outDir)/AppIcon-1024.png", size: 1024)

// ---- menu bar template (18pt @1x/@2x, monochrome black on alpha) ----
for (size, name) in [(18, "MenuIcon.png"), (36, "MenuIcon@2x.png")] {
    let image = NSImage(size: NSSize(width: size, height: size), flipped: false) { _ in
        let s = CGFloat(size)
        let wave = wavePath(s)
        wave.lineWidth = 0.09 * s
        wave.lineCapStyle = .round
        NSColor.black.setStroke()
        wave.stroke()
        drawBars(s, color: .black, ledGaps: false)
        return true
    }
    savePng(image, "\(outDir)/\(name)", size: size)
}
print("icons rendered to \(outDir)")
