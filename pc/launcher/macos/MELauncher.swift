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
    var renderScale = 100    // percent of the window the game renders at (ME_PC_RENDER_SCALE)
    var fullscreen = false   // ME_PC_FULLSCREEN
    var vsync = true         // ME_PC_VSYNC
    var fpsLimit = 0         // DXVK d3d9.maxFrameRate, 0 = none
    var anisotropy = 0       // DXVK d3d9.samplerAnisotropy, 0 = the game's choice
    var metalHud = false     // MTL_HUD_ENABLED
    var dxvkHud = ""         // DXVK_HUD, e.g. "fps,frametimes"
    // game
    var physx = false        // PhysXEnhanced in TdEngine.ini
    var shatter = true       // ME_PC_SHATTER: the native solvers
    var shatterCloth = true  // PXSHIM_CLOTH
    var fov = 0              // ME_PC_FOV, 0 = default
    var mouseCapture = true  // ME_PC_MOUSE_CAPTURE
    var guestCpus = 0        // ME_PC_CPUS, 0 = default
    // controller
    var padMap: [String: String] = [:]   // physical button -> reported button
    var padInvertY = false
    // diagnostics
    var frameLog = true      // ME_PC_FRAME_LOG
    var logPath = ""         // ME_PC_LOG_PATH
    var extraEnv = ""        // KEY=VALUE per line
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
        if let v = try c.decodeIfPresent(type(of: resX), forKey: .resX) { resX = v }
        if let v = try c.decodeIfPresent(type(of: resY), forKey: .resY) { resY = v }
        if let v = try c.decodeIfPresent(type(of: renderScale), forKey: .renderScale) { renderScale = v }
        if let v = try c.decodeIfPresent(type(of: vsync), forKey: .vsync) { vsync = v }
        if let v = try c.decodeIfPresent(type(of: fpsLimit), forKey: .fpsLimit) { fpsLimit = v }
        if let v = try c.decodeIfPresent(type(of: anisotropy), forKey: .anisotropy) { anisotropy = v }
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
    }
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
        if let d = try? Data(contentsOf: url), let s = try? JSONDecoder().decode(Settings.self, from: d) { return s }
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
            if s.meBinary.isEmpty { s.meBinary = pick(["\(w)/pc-recomp-upstream/bin-macos/me_pc", "\(w)/pc-recomp/bin-macos/me_pc"]) }
            if s.gameDir.isEmpty { s.gameDir = pick(["\(w)/pc/Binaries"]) }
            if s.depsDir.isEmpty { s.depsDir = pick(["\(w)/pc-deps"]) }
            if s.userDir.isEmpty { s.userDir = pick(["\(w)/pc-user"]) }
            if s.dxvkLib.isEmpty { s.dxvkLib = pick(["\(w)/dxvk-defer/bin/libdxvk_d3d9.0.dylib"]) }
        }
        if s.dxvkLib.isEmpty, let e = ProcessInfo.processInfo.environment["ME_PC_DXVK"] { s.dxvkLib = e }
        return s
    }
}

// ---- applying the settings ----
func environment(for s: Settings) -> [String: String] {
    var e: [String: String] = [:]
    if !s.dxvkLib.isEmpty { e["ME_PC_DXVK"] = s.dxvkLib }
    if s.renderScale != 100 { e["ME_PC_RENDER_SCALE"] = String(format: "%.2f", Double(s.renderScale) / 100) }
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
    if s.fov > 0 { e["ME_PC_FOV"] = String(s.fov) }
    if !s.mouseCapture { e["ME_PC_MOUSE_CAPTURE"] = "0" }
    if s.guestCpus > 0 { e["ME_PC_CPUS"] = String(s.guestCpus) }
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
func setIni(_ text: String, _ values: [String: String]) -> String {
    let crlf = text.contains("\r\n")
    var lines = text.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
    guard let start = lines.firstIndex(of: "[SystemSettings]") else { return text }
    var end = lines.count
    for i in (start + 1)..<lines.count where lines[i].hasPrefix("[") { end = i; break }
    for (k, v) in values {
        if let i = (start + 1..<end).first(where: { lines[$0].lowercased().hasPrefix(k.lowercased() + "=") }) {
            lines[i] = "\(k)=\(v)"
        } else { lines.insert("\(k)=\(v)", at: start + 1); end += 1 }
    }
    return lines.joined(separator: crlf ? "\r\n" : "\n")
}

func applyIni(_ s: Settings) -> String {
    let path = iniPath(s)
    guard let data = FileManager.default.contents(atPath: path), let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) else {
        return "TdEngine.ini not found yet (\(path)); the game creates it on its first start, then launch again to apply PhysX and resolution."
    }
    var v = ["PhysXEnhanced": s.physx ? "True" : "False"]
    if s.resX > 0 && s.resY > 0 { v["ResX"] = String(s.resX); v["ResY"] = String(s.resY) }
    let out = setIni(text, v)
    if out != text {
        let bak = path + ".launcher-bak"
        if !FileManager.default.fileExists(atPath: bak) { try? data.write(to: URL(fileURLWithPath: bak)) }
        try? out.data(using: .utf8)?.write(to: URL(fileURLWithPath: path))
        return "TdEngine.ini updated (backup: TdEngine.ini.launcher-bak)."
    }
    return "TdEngine.ini already matches."
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
    var body: some View {
        VStack(spacing: 0) {
            TabView {
                Form {
                    Section("Display") {
                        Picker("Resolution", selection: Binding(get: { "\(s.resX)x\(s.resY)" }, set: { v in
                            let p = v.split(separator: "x").compactMap { Int($0) }; s.resX = p.first ?? 0; s.resY = p.last ?? 0 })) {
                            Text("Keep the game's setting").tag("0x0")
                            ForEach(["1280x720", "1280x800", "1600x900", "1920x1080", "2560x1440", "2560x1600", "3024x1964"], id: \.self) { Text($0).tag($0) }
                        }
                        HStack { Text("Render scale"); Slider(value: Binding(get: { Double(s.renderScale) }, set: { s.renderScale = Int($0 / 5) * 5 }), in: 25...100); Text("\(s.renderScale)%").frame(width: 44) }
                        Text("The game renders at this fraction of the window and DXVK scales it up. Lower = faster, softer.").font(.caption).foregroundStyle(.secondary)
                        Toggle("Full screen (Cmd+Return toggles in game)", isOn: $s.fullscreen)
                        Toggle("Vsync (recommended on a 120 Hz screen)", isOn: $s.vsync)
                        Picker("Frame limit", selection: $s.fpsLimit) { Text("None").tag(0); Text("30").tag(30); Text("60").tag(60); Text("90").tag(90); Text("120").tag(120) }
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
                    }
                    Section("Game") {
                        Stepper("Field of view: \(s.fov == 0 ? "game default" : "\(s.fov)°")", value: $s.fov, in: 0...140, step: 5)
                        Toggle("Capture the mouse while the window has focus", isOn: $s.mouseCapture)
                        Stepper("CPUs the game sees: \(s.guestCpus == 0 ? "default" : "\(s.guestCpus)")", value: $s.guestCpus, in: 0...8)
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
                Button("Play") { Store.save(s); status = launch(s) }.keyboardShortcut(.defaultAction)
            }.padding(12)
        }.frame(minWidth: 620, minHeight: 520)
    }
}

struct LauncherApp: App {
    var body: some Scene { WindowGroup("Mirror's Edge – macOS launcher") { ContentView() } }
}

// ---- entry: headless modes first ----
@main struct Entry {
    static func main() {
        let args = CommandLine.arguments
        guard args.contains("--print") || args.contains("--launch") || args.contains("--apply-ini") else { LauncherApp.main(); return }
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
