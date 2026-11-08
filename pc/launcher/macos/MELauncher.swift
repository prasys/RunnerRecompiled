// Launcher for the macOS build: collects the game's options, applies them (environment for the runtime, TdEngine.ini
// for the game's own settings) and starts me_pc. Settings live in ~/Library/Application Support/RunnerRecompiled/launcher.json.
// Build: pc/launcher/macos/build.sh. Headless: `MELauncher --print` shows the command and environment, `--launch` starts
// the game without a window, `--apply-ini` only writes TdEngine.ini.
import SwiftUI
import AppKit

struct Settings: Codable, Equatable {
    // paths
    var meBinary = ""        // me_pc
    var gameDir = ""         // Binaries/ of the extracted game
    var depsDir = ""         // msvcr80.dll etc.
    var userDir = ""         // what the game writes
    var dxvkLib = ""         // libdxvk_d3d9*.dylib
    // graphics
    var resX = 0             // 0 = leave the game's TdEngine.ini alone
    var resY = 0
    var upscaler = "dxvk"    // native | dxvk (plain scaling) | metalfx -> ME_PC_UPSCALER
    var scalePreset = "custom" // native | quality | balanced | performance | ultra | custom (sets renderScale)
    var renderScale = 100    // percent of the window the game renders at (ME_PC_RENDER_SCALE)
    var fullscreen = false   // ME_PC_FULLSCREEN
    var vsync = true         // ME_PC_VSYNC
    var fpsLimit = 0         // DXVK d3d9.maxFrameRate, 0 = none
    var gameFps = 0          // MaxSmoothedFrameRate in TdEngine.ini [Engine.GameEngine]; 0 = leave, 1000 = unlimited
    var anisotropy = 0       // DXVK d3d9.samplerAnisotropy, 0 = the game's choice
    var metalHud = false     // MTL_HUD_ENABLED
    var dxvkHud = ""         // DXVK_HUD, e.g. "fps,frametimes"
    // game
    var physx = false        // PhysXEnhanced in TdEngine.ini
    var shatter = true       // ME_PC_SHATTER: the native solvers
    var shatterCloth = true  // PXSHIM_CLOTH
    var slideFix = false     // PXSHIM_SLIDEFIX: keep slide steering working above 62 fps (needs a runtime with the port, #5)
    var fov = 0              // ME_PC_FOV, 0 = default
    var mouseCapture = true  // ME_PC_MOUSE_CAPTURE
    var guestCpus = 0        // ME_PC_CPUS, 0 = default
    var skipStartupMovie = false // comments out StartupMovies in TdEngine.ini [FullScreenMovie]
    var keepEngineSleep = false  // ME_PC_KEEP_SLEEP1000: keep the engine's fixed 1 s sleep before the rendering thread restarts
    var pipelineWarmup = true    // DXVK_PIPELINE_WARM (off = 0): record and replay pipelines
    var warmWaitMs = 0           // DXVK_PIPELINE_WARM_WAIT_MS cap on the present hold, 0 = default
    var logWaitsMs = 0           // ME_PC_LOG_WAITS: log guest waits/sleeps at least this long, 0 = off
    // controller
    var padMap: [String: String] = [:]   // physical button -> reported button
    var padInvertY = false
    // diagnostics
    var frameLog = true      // ME_PC_FRAME_LOG
    var logPath = ""         // ME_PC_LOG_PATH
    var extraEnv = ""        // KEY=VALUE per line
    // look
    var backdropDir = ""     // folder of images for the banner; empty = Application Support/RunnerRecompiled/backdrops
    init() {}
    // Tolerant decoding: a missing key (older file) keeps its default.
    init(from d: Decoder) throws {
        self.init()
        let c = try d.container(keyedBy: CodingKeys.self)
        if let v = try c.decodeIfPresent(type(of: meBinary), forKey: .meBinary) { meBinary = v }
        if let v = try c.decodeIfPresent(type(of: gameDir), forKey: .gameDir) { gameDir = v }
        if let v = try c.decodeIfPresent(type(of: depsDir), forKey: .depsDir) { depsDir = v }
        if let v = try c.decodeIfPresent(type(of: userDir), forKey: .userDir) { userDir = v }
        if let v = try c.decodeIfPresent(type(of: dxvkLib), forKey: .dxvkLib) { dxvkLib = v }
        if let v = try c.decodeIfPresent(type(of: upscaler), forKey: .upscaler) { upscaler = v }
        if let v = try c.decodeIfPresent(type(of: resX), forKey: .resX) { resX = v }
        if let v = try c.decodeIfPresent(type(of: resY), forKey: .resY) { resY = v }
        if let v = try c.decodeIfPresent(type(of: scalePreset), forKey: .scalePreset) { scalePreset = v }
        if let v = try c.decodeIfPresent(type(of: renderScale), forKey: .renderScale) { renderScale = v }
        if let v = try c.decodeIfPresent(type(of: vsync), forKey: .vsync) { vsync = v }
        if let v = try c.decodeIfPresent(type(of: fpsLimit), forKey: .fpsLimit) { fpsLimit = v }
        if let v = try c.decodeIfPresent(type(of: anisotropy), forKey: .anisotropy) { anisotropy = v }
        if let v = try c.decodeIfPresent(type(of: gameFps), forKey: .gameFps) { gameFps = v }
        if let v = try c.decodeIfPresent(type(of: slideFix), forKey: .slideFix) { slideFix = v }
        if let v = try c.decodeIfPresent(type(of: skipStartupMovie), forKey: .skipStartupMovie) { skipStartupMovie = v }
        if let v = try c.decodeIfPresent(type(of: keepEngineSleep), forKey: .keepEngineSleep) { keepEngineSleep = v }
        if let v = try c.decodeIfPresent(type(of: pipelineWarmup), forKey: .pipelineWarmup) { pipelineWarmup = v }
        if let v = try c.decodeIfPresent(type(of: warmWaitMs), forKey: .warmWaitMs) { warmWaitMs = v }
        if let v = try c.decodeIfPresent(type(of: logWaitsMs), forKey: .logWaitsMs) { logWaitsMs = v }
        if let v = try c.decodeIfPresent(type(of: metalHud), forKey: .metalHud) { metalHud = v }
        if let v = try c.decodeIfPresent(type(of: dxvkHud), forKey: .dxvkHud) { dxvkHud = v }
        if let v = try c.decodeIfPresent(type(of: physx), forKey: .physx) { physx = v }
        if let v = try c.decodeIfPresent(type(of: shatter), forKey: .shatter) { shatter = v }
        if let v = try c.decodeIfPresent(type(of: shatterCloth), forKey: .shatterCloth) { shatterCloth = v }
        if let v = try c.decodeIfPresent(type(of: fov), forKey: .fov) { fov = v }
        if let v = try c.decodeIfPresent(type(of: mouseCapture), forKey: .mouseCapture) { mouseCapture = v }
        if let v = try c.decodeIfPresent(type(of: guestCpus), forKey: .guestCpus) { guestCpus = v }
        if let v = try c.decodeIfPresent(type(of: padMap), forKey: .padMap) { padMap = v }
        if let v = try c.decodeIfPresent(type(of: padInvertY), forKey: .padInvertY) { padInvertY = v }
        if let v = try c.decodeIfPresent(type(of: frameLog), forKey: .frameLog) { frameLog = v }
        if let v = try c.decodeIfPresent(type(of: logPath), forKey: .logPath) { logPath = v }
        if let v = try c.decodeIfPresent(type(of: extraEnv), forKey: .extraEnv) { extraEnv = v }
        if let v = try c.decodeIfPresent(type(of: backdropDir), forKey: .backdropDir) { backdropDir = v }
    }
}

/// Quality presets: share of the output resolution the game renders at (same idea as FSR/MetalFX quality modes).
let scalePresets: [(id: String, name: String, percent: Int)] = [
    ("native", "Native", 100), ("quality", "Quality", 77), ("balanced", "Balanced", 67),
    ("performance", "Performance", 50), ("ultra", "Ultra Performance", 33)]

/// The resolution the game itself renders at. With an output resolution chosen, it is output x scale (even numbers),
/// and the window stays at the output size because the runtime makes the window guest / scale. With "keep the game's
/// setting" the game's own ResX/ResY is left alone and the scale is applied to it (the window is capped to the display).
func guestSize(_ s: Settings) -> (Int, Int)? {
    guard let o = outputSize(s) else { return nil }
    let f = s.upscaler == "native" ? 1.0 : Double(s.renderScale) / 100
    if f == 1.0 { return o }   // no scaling: exactly the output size (rounding to even would resample by a pixel)
    func even(_ v: Double) -> Int { max(2, Int((v / 2).rounded()) * 2) }
    return (even(Double(o.0) * f), even(Double(o.1) * f))
}

/// The output (window) size: the chosen resolution, shrunk to fit the display (the game draws at the display's logical
/// size, so an output bigger than that is downscaled by the window cap and the upscalers cannot downscale).
func outputSize(_ s: Settings) -> (Int, Int)? {
    guard s.resX > 0, s.resY > 0 else { return nil }
    var w = Double(s.resX), h = Double(s.resY)
    if let d = NSScreen.main?.frame.size, w > d.width || h > d.height {
        let f = min(d.width / w, d.height / h); w *= f; h *= f
    }
    return (Int(w.rounded()), Int(h.rounded()))
}

let padButtons = ["A", "B", "X", "Y", "LB", "RB", "LS", "RS", "START", "BACK", "UP", "DOWN", "LEFT", "RIGHT"]

enum Store {
    static var url: URL {
        if let o = ProcessInfo.processInfo.environment["ME_LAUNCHER_CONFIG"] { return URL(fileURLWithPath: o) } // tests
        let d = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RunnerRecompiled")
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d.appendingPathComponent("launcher.json")
    }
    static func load() -> Settings {
        // A saved file from an older launcher may lack newer paths: fill only the empty ones.
        if let d = try? Data(contentsOf: url), let s = try? JSONDecoder().decode(Settings.self, from: d) { return autodetect(s) }
        return autodetect(Settings())
    }
    static func save(_ s: Settings) {
        let e = JSONEncoder(); e.outputFormatting = [.prettyPrinted, .sortedKeys]
        try? e.encode(s).write(to: url)
    }
    // Fill empty paths from the repo layout (work/...) when the launcher sits in <repo>/work or the repo is the cwd.
    static func autodetect(_ s: Settings) -> Settings {
        var s = s
        let fm = FileManager.default
        var roots = [fm.currentDirectoryPath]
        var p = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        for _ in 0..<6 { p.deleteLastPathComponent(); roots.append(p.path) }
        for r in roots {
            let w = r + "/work"
            func pick(_ cands: [String]) -> String { cands.first { fm.fileExists(atPath: $0) } ?? "" }
            if s.meBinary.isEmpty { s.meBinary = pick(["\(w)/pc-recomp-slide/bin-macos/me_pc", "\(w)/pc-recomp-modes/bin-macos/me_pc", "\(w)/pc-recomp-upstream/bin-macos/me_pc", "\(w)/pc-recomp/bin-macos/me_pc"]) }
            if s.gameDir.isEmpty { s.gameDir = pick(["\(w)/pc/Binaries"]) }
            if s.depsDir.isEmpty { s.depsDir = pick(["\(w)/pc-deps"]) }
            if s.userDir.isEmpty { s.userDir = pick(["\(w)/pc-user"]) }
            if s.dxvkLib.isEmpty { s.dxvkLib = pick(["\(w)/dxvk/bin/libdxvk_d3d9.0.dylib", "\(w)/dxvk-defer/bin/libdxvk_d3d9.0.dylib"]) }
        }
        if s.dxvkLib.isEmpty, let e = ProcessInfo.processInfo.environment["ME_PC_DXVK"] { s.dxvkLib = e }
        return s
    }
}

// ---- applying the settings ----
func environment(for s: Settings) -> [String: String] {
    var e: [String: String] = [:]
    if !s.dxvkLib.isEmpty { e["ME_PC_DXVK"] = s.dxvkLib }
    // Upscaling: "native" renders at the window size (no scaling); "dxvk" is the plain scale-up; "metalfx" needs the
    // same DXVK build as the other modes (MetalFX is compiled into pc/dxvk/dxvk-3.1.1-macos.patch; an older build ignores it).
    if let g = guestSize(s) { e["ME_PC_EXTRA_MODES"] = "\(g.0)x\(g.1)" }
    if s.upscaler == "metalfx" {
        e["ME_PC_UPSCALER"] = "metalfx"
    }
    if s.upscaler != "native" && s.renderScale != 100 { e["ME_PC_RENDER_SCALE"] = String(format: "%.2f", Double(s.renderScale) / 100) }
    if s.fullscreen { e["ME_PC_FULLSCREEN"] = "1" }
    if !s.vsync { e["ME_PC_VSYNC"] = "0" }
    var dxvk: [String] = []
    if s.fpsLimit > 0 { dxvk.append("d3d9.maxFrameRate = \(s.fpsLimit)") }
    if s.anisotropy > 0 { dxvk.append("d3d9.samplerAnisotropy = \(s.anisotropy)") }
    if !dxvk.isEmpty { e["DXVK_CONFIG"] = dxvk.joined(separator: "; ") }
    if s.metalHud { e["MTL_HUD_ENABLED"] = "1" }
    if !s.dxvkHud.isEmpty { e["DXVK_HUD"] = s.dxvkHud }
    if s.physx && !s.shatter { e["ME_PC_SHATTER"] = "0" }
    if s.physx && s.shatter && !s.shatterCloth { e["PXSHIM_CLOTH"] = "0" }
    if s.slideFix { e["PXSHIM_SLIDEFIX"] = "1" }
    if s.fov > 0 { e["ME_PC_FOV"] = String(s.fov) }
    if !s.mouseCapture { e["ME_PC_MOUSE_CAPTURE"] = "0" }
    if s.guestCpus > 0 { e["ME_PC_CPUS"] = String(s.guestCpus) }
    if s.keepEngineSleep { e["ME_PC_KEEP_SLEEP1000"] = "1" }
    if !s.pipelineWarmup { e["DXVK_PIPELINE_WARM"] = "0" }
    if s.warmWaitMs > 0 { e["DXVK_PIPELINE_WARM_WAIT_MS"] = String(s.warmWaitMs) }
    if s.logWaitsMs > 0 { e["ME_PC_LOG_WAITS"] = String(s.logWaitsMs) }
    let map = s.padMap.filter { $0.key != $0.value }.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }
    if !map.isEmpty { e["ME_PC_PAD_MAP"] = map.joined(separator: ",") }
    if s.padInvertY { e["ME_PC_PAD_INVERT_Y"] = "1" }
    if !s.frameLog { e["ME_PC_FRAME_LOG"] = "0" }
    if !s.logPath.isEmpty { e["ME_PC_LOG_PATH"] = s.logPath }
    for line in s.extraEnv.split(whereSeparator: \.isNewline) {
        if let eq = line.firstIndex(of: "="), eq != line.startIndex {
            e[String(line[..<eq]).trimmingCharacters(in: .whitespaces)] = String(line[line.index(after: eq)...])
        }
    }
    return e
}

func iniPath(_ s: Settings) -> String {
    s.userDir + "/other/C/Documents and Settings/Runner/My Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini"
}

/// Sets Key=Value inside [SystemSettings] (adds the line if missing). Keeps the file's line endings. Returns a message.
func setIni(_ text: String, _ values: [String: String], section: String = "[SystemSettings]") -> String {
    let crlf = text.contains("\r\n")
    var lines = text.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
    guard let start = lines.firstIndex(of: section) else { return text }
    var end = lines.count
    for i in (start + 1)..<lines.count where lines[i].hasPrefix("[") { end = i; break }
    for (k, v) in values {
        if let i = (start + 1..<end).first(where: { lines[$0].lowercased().hasPrefix(k.lowercased() + "=") }) {
            lines[i] = "\(k)=\(v)"
        } else { lines.insert("\(k)=\(v)", at: start + 1); end += 1 }
    }
    return lines.joined(separator: crlf ? "\r\n" : "\n")
}

/// Comments out (or restores) `StartupMovies=` in [FullScreenMovie]; only touches that section.
func setStartupMovie(_ text: String, skip: Bool) -> String {
    let crlf = text.contains("\r\n")
    var lines = text.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
    guard let start = lines.firstIndex(of: "[FullScreenMovie]") else { return text }
    var end = lines.count
    for i in (start + 1)..<lines.count where lines[i].hasPrefix("[") { end = i; break }
    for i in start + 1..<end {
        if skip, lines[i].lowercased().hasPrefix("startupmovies=") { lines[i] = ";" + lines[i] }
        else if !skip, lines[i].lowercased().hasPrefix(";startupmovies=") { lines[i].removeFirst() }
    }
    return lines.joined(separator: crlf ? "\r\n" : "\n")
}

func applyIni(_ s: Settings) -> String {
    let path = iniPath(s)
    guard let data = FileManager.default.contents(atPath: path), let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) else {
        return "TdEngine.ini not found yet (\(path)); the game creates it on its first start, then launch again to apply PhysX and resolution."
    }
    var v = ["PhysXEnhanced": s.physx ? "True" : "False"]
    if let g = guestSize(s) { v["ResX"] = String(g.0); v["ResY"] = String(g.1) }
    var out = setIni(text, v)
    out = setStartupMovie(out, skip: s.skipStartupMovie)
    if s.gameFps > 0 { out = setIni(out, ["bSmoothFrameRate": "TRUE", "MaxSmoothedFrameRate": String(s.gameFps)], section: "[Engine.GameEngine]") }
    if out != text {
        let bak = path + ".launcher-bak"
        if !FileManager.default.fileExists(atPath: bak) { try? data.write(to: URL(fileURLWithPath: bak)) }
        try? out.data(using: .utf8)?.write(to: URL(fileURLWithPath: path))
        return "TdEngine.ini updated (backup: TdEngine.ini.launcher-bak)."
    }
    return "TdEngine.ini already matches."
}

/// Deletes the pipeline warm-up files (records, saved shader bytecode, backups) so the next start is cold. With
/// `shaderCache` it also deletes DXVK's own shader cache (`*.dxvk.bin`, `*.dxvk.lut`) for a fully cold start.
func clearWarmData(shaderCache: Bool) -> String {
    let fm = FileManager.default
    let dir = (ProcessInfo.processInfo.environment["HOME"] ?? NSHomeDirectory()) + "/.cache/dxvk"
    let custom = ProcessInfo.processInfo.environment["DXVK_PIPELINE_WARM_FILE"]
    var removed = 0, bytes: UInt64 = 0
    func remove(_ path: String) {
        if let a = try? fm.attributesOfItem(atPath: path), (a[.type] as? FileAttributeType) == .typeRegular {
            bytes += (a[.size] as? UInt64) ?? 0
            if (try? fm.removeItem(atPath: path)) != nil { removed += 1 }
        }
    }
    for base in [dir + "/pipelines.warm"] + (custom.map { [$0] } ?? []) {
        for suffix in ["", ".shaders", ".old", ".old.shaders"] { remove(base + suffix) }
    }
    if shaderCache, let names = try? fm.contentsOfDirectory(atPath: dir) {
        for n in names where n.hasSuffix(".dxvk.bin") || n.hasSuffix(".dxvk.lut") { remove(dir + "/" + n) }
    }
    return removed == 0 ? "Nothing to clear in \(dir)." : "Cleared \(removed) file(s), \(bytes / 1024) KB, from \(dir). The next start is cold."
}

func problems(_ s: Settings) -> [String] {
    let fm = FileManager.default
    var p: [String] = []
    if !fm.isExecutableFile(atPath: s.meBinary) { p.append("me_pc not found") }
    if !fm.fileExists(atPath: s.gameDir + "/MirrorsEdge.exe") { p.append("game folder has no MirrorsEdge.exe") }
    if !fm.fileExists(atPath: s.depsDir) { p.append("deps folder not found") }
    if !fm.fileExists(atPath: s.userDir) { p.append("user folder not found") }
    if !fm.fileExists(atPath: s.dxvkLib) { p.append("DXVK library not found") }
    return p
}

var running: Process?

func launch(_ s: Settings) -> String {
    let bad = problems(s)
    if !bad.isEmpty { return "Cannot start: " + bad.joined(separator: "; ") }
    let msg = applyIni(s)
    let p = Process()
    p.executableURL = URL(fileURLWithPath: s.meBinary)
    p.arguments = [s.gameDir, s.depsDir, s.userDir]
    var env = ProcessInfo.processInfo.environment
    for (k, v) in environment(for: s) { env[k] = v }
    p.environment = env
    p.currentDirectoryURL = URL(fileURLWithPath: s.userDir)
    do { try p.run() } catch { return "Launch failed: \(error)" }
    running = p
    return msg + " Game started (pid \(p.processIdentifier))."
}


// ---- banner art ----
let faithRed = Color(red: 0.93, green: 0.11, blue: 0.14)

/// Folder the banner reads images from (png/jpg/jpeg/heic/tiff). Created on demand so "Open folder" always works.
func backdropFolder(_ s: Settings) -> URL {
    if !s.backdropDir.isEmpty { return URL(fileURLWithPath: s.backdropDir) }
    let d = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("RunnerRecompiled/backdrops")
    try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
    return d
}

func backdropImages(_ s: Settings) -> [NSImage] {
    let exts: Set<String> = ["png", "jpg", "jpeg", "heic", "tif", "tiff"]
    let files = (try? FileManager.default.contentsOfDirectory(at: backdropFolder(s), includingPropertiesForKeys: nil)) ?? []
    return files.filter { exts.contains($0.pathExtension.lowercased()) }.sorted { $0.path < $1.path }.compactMap { NSImage(contentsOf: $0) }
}

/// Built-in banner: a pale skyline with one red building and a runner-vision line, drawn (no game assets involved).
struct SkylineArt: View {
    var body: some View {
        Canvas { ctx, size in
            ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .linearGradient(
                Gradient(colors: [Color(red: 0.74, green: 0.85, blue: 0.95), Color(red: 0.97, green: 0.98, blue: 1.0)]),
                startPoint: .zero, endPoint: CGPoint(x: 0, y: size.height)))
            var seed: UInt64 = 7
            func rnd() -> Double { seed = seed &* 6364136223846793005 &+ 1442695040888963407; return Double(seed >> 33) / Double(1 << 31) }
            let layers: [(Double, Double, Double)] = [(0.50, 0.93, 0.75), (0.38, 0.86, 0.9), (0.22, 0.78, 1.0)] // top frac, brightness, alpha
            for (i, l) in layers.enumerated() {
                var x: Double = -10
                let shade: Double = l.1
                while x < size.width {
                    let w: Double = 28 + rnd() * 60
                    let h: Double = size.height * (1 - l.0) * (0.45 + rnd() * 0.55)
                    let redX: Double = size.width * 0.66
                    let red: Bool = i == 2 && x > redX && x < redX + 80
                    let r = CGRect(x: x, y: size.height - h, width: w, height: h)
                    ctx.fill(Path(r), with: .color(red ? faithRed : Color(white: shade, opacity: l.2)))
                    if !red && rnd() > 0.5 { // a few lit windows
                        for _ in 0..<3 {
                            let wx: Double = x + 4 + rnd() * (w - 10)
                            let wy: Double = size.height - h + 6 + rnd() * (h - 12)
                            ctx.fill(Path(CGRect(x: wx, y: wy, width: 3, height: 5)), with: .color(.white.opacity(0.7)))
                        }
                    }
                    x += w + rnd() * 6
                }
            }
            var line = Path()
            line.move(to: CGPoint(x: size.width * 0.05, y: size.height * 0.92))
            line.addLine(to: CGPoint(x: size.width * 0.58, y: size.height * 0.52))
            line.addLine(to: CGPoint(x: size.width * 0.95, y: size.height * 0.30))
            ctx.stroke(line, with: .color(faithRed), style: StrokeStyle(lineWidth: 3, lineCap: .round))
        }
    }
}

/// Header: crossfading, slowly zooming images from the backdrops folder (or the built-in art), title and Play on top.
struct Banner: View {
    let settings: Settings
    let status: String
    let play: () -> Void
    @State var images: [NSImage] = []
    @State var index = 0
    @State var zoom = false
    let tick = Timer.publish(every: 9, on: .main, in: .common).autoconnect()

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            Group {
                if images.isEmpty { SkylineArt() }
                else {
                    Image(nsImage: images[index % images.count]).resizable().scaledToFill()
                        .scaleEffect(zoom ? 1.08 : 1.0).id(index)
                        .transition(.opacity)
                }
            }.frame(maxWidth: .infinity).frame(height: 190).clipped()
            LinearGradient(colors: [.clear, .black.opacity(0.65)], startPoint: .center, endPoint: .bottom)
            HStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("MIRROR'S EDGE").font(.system(size: 30, weight: .heavy)).tracking(4).foregroundStyle(.white)
                    Text(images.isEmpty ? "macOS · lifted from the original PC build" : "macOS · \(images.count) backdrop\(images.count == 1 ? "" : "s")")
                        .font(.caption).foregroundStyle(.white.opacity(0.85))
                }
                Spacer()
                Button(action: play) {
                    Text("PLAY").font(.system(size: 16, weight: .heavy)).tracking(3).padding(.horizontal, 26).padding(.vertical, 8)
                }.buttonStyle(.plain).background(faithRed, in: RoundedRectangle(cornerRadius: 6)).foregroundStyle(.white)
                    .keyboardShortcut(.defaultAction)
            }.padding(16)
        }
        .frame(height: 190)
        .onAppear { reload() }
        .onChange(of: settings.backdropDir) { _ in reload() }
        .onReceive(tick) { _ in
            guard images.count > 1 else { return }
            withAnimation(.easeInOut(duration: 1.2)) { index += 1 }
            zoom = false; withAnimation(.linear(duration: 10)) { zoom = true }
        }
    }
    func reload() {
        images = backdropImages(settings); index = 0
        zoom = false; withAnimation(.linear(duration: 10)) { zoom = true }
    }
}

// ---- UI ----
struct PathRow: View {
    let label: String
    @Binding var value: String
    var directory = true
    var body: some View {
        HStack {
            Text(label).frame(width: 110, alignment: .trailing)
            TextField("", text: $value).textFieldStyle(.roundedBorder)
            Button("Choose…") {
                let o = NSOpenPanel(); o.canChooseFiles = !directory; o.canChooseDirectories = directory
                if o.runModal() == .OK, let u = o.url { value = u.path }
            }
        }
    }
}

struct ContentView: View {
    @State var s = Store.load()
    @State var status = ""
    @State var clearShaderCache = false
    var body: some View {
        VStack(spacing: 0) {
            Banner(settings: s, status: status) { Store.save(s); status = launch(s) }
            TabView {
                Form {
                    Section("Display") {
                        Picker("Output resolution", selection: Binding(get: { "\(s.resX)x\(s.resY)" }, set: { v in
                            let p = v.split(separator: "x").compactMap { Int($0) }; s.resX = p.first ?? 0; s.resY = p.last ?? 0 })) {
                            Text("Keep the game's setting").tag("0x0")
                            if let sc = NSScreen.main {
                                let d = sc.frame.size, k = sc.backingScaleFactor
                                Text(k > 1 ? "This display, drawn at \(Int(d.width))x\(Int(d.height)) (panel \(Int(d.width * k))x\(Int(d.height * k)))" : "This display (\(Int(d.width))x\(Int(d.height)))")
                                    .tag("\(Int(d.width))x\(Int(d.height))")
                            }
                            ForEach(["1280x720", "1280x800", "1600x900", "1920x1080", "2560x1440", "2560x1600", "3024x1964"].filter { r in
                                let p = r.split(separator: "x").compactMap { Double($0) }
                                guard let d = NSScreen.main?.frame.size, p.count == 2 else { return true }
                                return p[0] <= d.width && p[1] <= d.height }, id: \.self) { Text($0).tag($0) }
                        }
                        Text("On a Retina screen the game draws at the logical size (macOS then doubles it); true panel resolution is not used yet.").font(.caption).foregroundStyle(.secondary)
                        Picker("Upscaling", selection: $s.upscaler) {
                            Text("Native (no scaling)").tag("native"); Text("Standard (DXVK)").tag("dxvk"); Text("MetalFX (spatial, experimental)").tag("metalfx")
                        }
                        Picker("Quality", selection: Binding(get: { s.scalePreset }, set: { v in
                            s.scalePreset = v; if let p = scalePresets.first(where: { $0.id == v }) { s.renderScale = p.percent } })) {
                            ForEach(scalePresets, id: \.id) { Text("\($0.name) (\($0.percent)%)").tag($0.id) }
                            Text("Custom").tag("custom")
                        }.disabled(s.upscaler == "native")
                        if let g = guestSize(s) {
                            Text("The game renders at \(g.0)x\(g.1) and is scaled to \(outputSize(s)?.0 ?? s.resX)x\(outputSize(s)?.1 ?? s.resY)\((outputSize(s)?.0 ?? s.resX) != s.resX ? " (your \(s.resX)x\(s.resY) is bigger than the display, so it is capped)" : "").").font(.caption).foregroundStyle(.secondary)
                        }
                        if s.scalePreset == "custom" { HStack { Text("Render scale"); Slider(value: Binding(get: { Double(s.renderScale) }, set: { s.renderScale = Int($0 / 5) * 5 }), in: 25...100); Text("\(s.renderScale)%").frame(width: 44) }.disabled(s.upscaler == "native") }
                        Text("Quality is the share of the output resolution the game renders at; the upscaler fills the rest. Lower = faster, softer.").font(.caption).foregroundStyle(.secondary)
                        Toggle("Full screen (Cmd+Return toggles in game)", isOn: $s.fullscreen)
                        Toggle("Vsync (recommended on a 120 Hz screen)", isOn: $s.vsync)
                        Picker("Game frame cap", selection: $s.gameFps) {
                            Text("Game's setting").tag(0); Text("60").tag(60); Text("62 (original)").tag(62); Text("90").tag(90)
                            Text("120").tag(120); Text("144").tag(144); Text("Unlimited").tag(1000)
                        }
                        if s.gameFps > 62 || s.gameFps == 1000 {
                            Text(s.vsync ? "With Vsync on, the frame rate stops at the screen's refresh rate; turn Vsync off to go past it (tearing)."
                                         : "Vsync is off: frames above the refresh rate tear.").font(.caption).foregroundStyle(.secondary)
                            Text("Above 62 fps the game loses left/right steering on steep slides unless the slide fix is on.").font(.caption).foregroundStyle(.secondary)
                        }
                        Picker("Extra limiter (DXVK)", selection: $s.fpsLimit) { Text("None").tag(0); Text("30").tag(30); Text("60").tag(60); Text("90").tag(90); Text("120").tag(120) }
                        Picker("Anisotropic filtering", selection: $s.anisotropy) { Text("Game's choice").tag(0); ForEach([2, 4, 8, 16], id: \.self) { Text("\($0)x").tag($0) } }
                    }
                    Section("Overlays") {
                        Toggle("Metal HUD (fps, GPU, memory)", isOn: $s.metalHud)
                        Picker("DXVK HUD", selection: $s.dxvkHud) {
                            Text("Off").tag(""); Text("fps").tag("fps"); Text("fps + frame times").tag("fps,frametimes")
                            Text("fps + GPU load + memory").tag("fps,gpuload,memory"); Text("everything useful").tag("fps,frametimes,gpuload,memory,pipelines,drawcalls")
                        }
                    }
                }.formStyle(.grouped).tabItem { Text("Graphics") }
                Form {
                    Section("PhysX effects") {
                        Toggle("Enhanced PhysX (glass, debris, smoke, cloth)", isOn: $s.physx)
                        Toggle("Run them on Shatter's native solvers (fast)", isOn: $s.shatter).disabled(!s.physx)
                        Toggle("Cloth on Shatter too", isOn: $s.shatterCloth).disabled(!s.physx || !s.shatter)
                        Toggle("Slide steering fix (for frame caps above 62)", isOn: $s.slideFix)
                        Text("Needs a runtime with the fix (issue #5, in progress); an older runtime ignores it.").font(.caption).foregroundStyle(.secondary)
                    }
                    Section("Game") {
                        Stepper("Field of view: \(s.fov == 0 ? "game default" : "\(s.fov)°")", value: $s.fov, in: 0...140, step: 5)
                        Toggle("Capture the mouse while the window has focus", isOn: $s.mouseCapture)
                        Stepper("CPUs the game sees: \(s.guestCpus == 0 ? "default" : "\(s.guestCpus)")", value: $s.guestCpus, in: 0...8)
                        Toggle("Skip the startup movie", isOn: $s.skipStartupMovie)
                    }
                    Section("Loading and stalls") {
                        Toggle("Keep the engine's fixed 1 s sleep (off removes the periodic 1 s freeze)", isOn: $s.keepEngineSleep)
                        Toggle("Pipeline warm-up (record and replay shader pipelines)", isOn: $s.pipelineWarmup)
                        Stepper("Warm-up wait cap: \(s.warmWaitMs == 0 ? "default" : "\(s.warmWaitMs) ms")", value: $s.warmWaitMs, in: 0...60000, step: 1000).disabled(!s.pipelineWarmup)
                        Stepper("Log waits of at least: \(s.logWaitsMs == 0 ? "off" : "\(s.logWaitsMs) ms")", value: $s.logWaitsMs, in: 0...5000, step: 100)
                        HStack {
                            Button("Clear warm-up data (cold start)") { status = clearWarmData(shaderCache: clearShaderCache) }
                            Toggle("also DXVK's shader cache", isOn: $clearShaderCache)
                        }
                        Text("Deletes the recorded pipelines and shader bytecode in ~/.cache/dxvk. Close the game first.").font(.caption).foregroundStyle(.secondary)
                    }
                }.formStyle(.grouped).tabItem { Text("Game") }
                Form {
                    Section("Button mapping (physical → what the game receives)") {
                        ForEach(padButtons, id: \.self) { b in
                            Picker(b, selection: Binding(get: { s.padMap[b] ?? b }, set: { s.padMap[b] = $0 })) { ForEach(padButtons, id: \.self) { Text($0).tag($0) } }
                        }
                        Toggle("Invert look (right stick Y)", isOn: $s.padInvertY)
                        Button("Reset mapping") { s.padMap = [:]; s.padInvertY = false }
                        Text("A/B/X/Y are the Xbox names: A = bottom, B = right, X = left, Y = top button.").font(.caption).foregroundStyle(.secondary)
                    }
                }.formStyle(.grouped).tabItem { Text("Controller") }
                VStack(alignment: .leading, spacing: 8) {
                    PathRow(label: "me_pc", value: $s.meBinary, directory: false)
                    PathRow(label: "Game (Binaries)", value: $s.gameDir)
                    PathRow(label: "Deps", value: $s.depsDir)
                    PathRow(label: "User folder", value: $s.userDir)
                    PathRow(label: "DXVK dylib", value: $s.dxvkLib, directory: false)
                    PathRow(label: "Log file", value: $s.logPath, directory: false)
                    PathRow(label: "Backdrops", value: $s.backdropDir)
                    HStack {
                        Spacer().frame(width: 110)
                        Button("Open backdrops folder") { NSWorkspace.shared.open(backdropFolder(s)) }
                        Text("Drop screenshots or wallpapers here; the banner cycles through them.").font(.caption).foregroundStyle(.secondary)
                    }
                    Toggle("Frame log (once a second)", isOn: $s.frameLog)
                    Text("Extra environment (KEY=VALUE per line)")
                    TextEditor(text: $s.extraEnv).font(.system(.body, design: .monospaced)).frame(height: 70).border(Color.gray.opacity(0.4))
                    Spacer()
                }.padding().tabItem { Text("Paths") }
            }.padding(.top, 8)
            Divider()
            HStack {
                Text(status).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                Spacer()
                Button("Save") { Store.save(s); status = "Saved." }
            }.padding(12)
        }.frame(minWidth: 680, minHeight: 640)
    }
}

struct LauncherApp: App {
    var body: some Scene { WindowGroup("Mirror's Edge – macOS launcher") { ContentView() } }
}

/// Renders the window's content to a PNG without showing it (`--snapshot out.png`), for checking the look headless.
@MainActor func snapshot(to path: String) {
    // ImageRenderer cannot draw TabView/Form (native controls), so this shows the banner only.
    let r = ImageRenderer(content: Banner(settings: Store.load(), status: "") {}.frame(width: 680))
    r.scale = 2
    if let img = r.nsImage, let t = img.tiffRepresentation, let b = NSBitmapImageRep(data: t)?.representation(using: .png, properties: [:]) {
        try? b.write(to: URL(fileURLWithPath: path)); print("wrote \(path)")
    } else { print("snapshot failed") }
}

// ---- entry: headless modes first ----
@main struct Entry {
    static func main() {
        let args = CommandLine.arguments
        guard args.contains("--print") || args.contains("--launch") || args.contains("--apply-ini") || args.contains("--snapshot") else { LauncherApp.main(); return }
        if let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count { snapshot(to: args[i + 1]); return }
        let s = Store.load()
        if args.contains("--print") {
            print("\(s.meBinary) \(s.gameDir) \(s.depsDir) \(s.userDir)")
            for (k, v) in environment(for: s).sorted(by: { $0.key < $1.key }) { print("\(k)=\(v)") }
            let bad = problems(s); if !bad.isEmpty { print("problems: " + bad.joined(separator: "; ")) }
        }
        if args.contains("--apply-ini") { print(applyIni(s)) }
        if args.contains("--launch") { print(launch(s)); running?.waitUntilExit() }
    }
}
