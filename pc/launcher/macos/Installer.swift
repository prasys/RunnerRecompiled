import Foundation
import CryptoKit

/// The install engine behind the launcher's first-run wizard and `MELauncher --install <setup.exe>`.
/// It turns the user's own GOG installer into a playable build under `base` (default ~/MirrorsEdge), following
/// pc/MACOS-BUILD.md steps 1-6. Nothing from the game leaves that folder. Every step is idempotent: install.json
/// remembers the finished ones, so a failed or cancelled install resumes.
///
/// Layout under `base`: pc/ (extracted game), pc-deps/ (CRT + rebased DLLs), pc-recomp/ (catalogs, lifted C,
/// objects, bin-macos/me_pc, lib/libSDL3), dxvk/bin/, pc-user/ (settings, saves), logs/, install.json.
enum InstallError: Error, CustomStringConvertible {
    case msg(String)
    var description: String { if case .msg(let m) = self { return m }; return "" }
}

struct InstallStep: Identifiable {
    let id: String
    let title: String
}

let installSteps: [InstallStep] = [
    InstallStep(id: "check", title: "Checking your Mac and the installer"),
    InstallStep(id: "extract", title: "Extracting the game from the GOG installer"),
    InstallStep(id: "runtime", title: "Extracting the Microsoft C runtime"),
    InstallStep(id: "rebase", title: "Preparing the game's DLLs"),
    InstallStep(id: "catalog", title: "Cataloguing the game's code"),
    InstallStep(id: "build", title: "Building the macOS game (a few minutes)"),
    InstallStep(id: "finish", title: "Finishing up"),
]

let gameExeSHA256 = "58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd"
let crtSHA256 = ["msvcr80.dll": "02bdde38e4c6bd795a092d496b8d6060cdbe71e22ef4d7a204e3050c1be44fa9",
                 "msvcp80.dll": "2026f3c4f830dff6883b88e2647272a52a132f25eb42c0d423e36b3f65a94d08"]
// Must match MODULES in pc/tools/build_pc_runtime.py.
let rebasedDLLs: [(String, String)] = [
    ("PhysXLoader", "20000000"), ("PhysXCore", "21000000"), ("NxCooking", "23000000"), ("PhysXExtensions", "24000000"),
    ("NxCharacter", "25000000"), ("ogg", "26000000"), ("vorbis", "27000000"), ("vorbisfile", "28000000"),
    ("vorbisenc", "29000000"), ("libresample", "2a000000"), ("binkw32", "2b000000"),
]

final class Installer {
    let base: URL
    let setupExe: URL
    /// (step index, fraction 0...1 or nil, message)
    var onProgress: (Int, Double?, String) -> Void = { _, _, _ in }
    private(set) var cancelled = false
    private var current: Process?
    private let fm = FileManager.default

    init(setupExe: URL, base: URL) { self.setupExe = setupExe; self.base = base }

    func cancel() { cancelled = true; current?.terminate() }

    // ---- where things are: the app bundle first (shipped), then Homebrew (developer machines) ----
    static var resources: URL? { Bundle.main.resourceURL }
    static func env(_ k: String) -> String? { ProcessInfo.processInfo.environment[k] }

    /// Our code: pc/tools, pc/runtime, pc/shatter. ME_INSTALL_SRC, the bundle's Resources/src, or the repo around the binary.
    static func sourceRoot() -> URL? {
        let fm = FileManager.default
        var cands: [URL] = []
        if let e = env("ME_INSTALL_SRC") { cands.append(URL(fileURLWithPath: e)) }
        if let r = resources { cands.append(r.appendingPathComponent("src")) }
        var p = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        for _ in 0..<7 { p.deleteLastPathComponent(); cands.append(p) }
        cands.append(URL(fileURLWithPath: fm.currentDirectoryPath))
        return cands.first { fm.fileExists(atPath: $0.appendingPathComponent("pc/tools/build_pc_runtime.py").path) }
    }
    static var vendor: URL? {
        if let e = env("ME_BUNDLE") { return URL(fileURLWithPath: e) }
        return resources?.appendingPathComponent("vendor")
    }
    static func tool(_ name: String) -> String? {
        var c: [String] = []
        if let v = vendor { c.append(v.appendingPathComponent("bin/\(name)").path) }
        c += ["/opt/homebrew/bin/\(name)", "/usr/local/bin/\(name)"]
        return c.first { FileManager.default.isExecutableFile(atPath: $0) }
    }
    static func firstExisting(_ c: [String?]) -> String? {
        c.compactMap { $0 }.first { FileManager.default.fileExists(atPath: $0) }
    }
    static var llvmLib: String? {
        firstExisting([vendor?.appendingPathComponent("llvm/lib").path, "/opt/homebrew/opt/llvm@23/lib"])
    }
    static var sdl3Prefix: String? {
        firstExisting([vendor?.appendingPathComponent("sdl3").path, "/opt/homebrew/opt/sdl3"])
    }
    static var dxvkDylib: String? {
        firstExisting([env("ME_INSTALL_DXVK"), vendor?.appendingPathComponent("lib/libdxvk_d3d9.0.dylib").path])
    }
    static let python = "/usr/bin/python3"   // the Command Line Tools' Python (3.9): all the lifter needs

    static func commandLineToolsInstalled() -> Bool {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select"); p.arguments = ["-p"]
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        do { try p.run(); p.waitUntilExit() } catch { return false }
        return p.terminationStatus == 0 && FileManager.default.fileExists(atPath: "/Library/Developer/CommandLineTools/usr/bin/clang")
            || p.terminationStatus == 0 && FileManager.default.fileExists(atPath: "/Applications/Xcode.app/Contents/Developer/usr/bin/clang")
    }

    /// Parts of the GOG installer that must sit next to the .exe: "<name>-1.bin", "-2.bin", ...
    static func missingParts(of exe: URL) -> [String] {
        let dir = exe.deletingLastPathComponent(), stem = exe.deletingPathExtension().lastPathComponent
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        let have = names.filter { $0.hasPrefix(stem + "-") && $0.hasSuffix(".bin") }
        return have.isEmpty ? ["\(stem)-1.bin"] : []
    }

    // ---- state ----
    var stateURL: URL { base.appendingPathComponent("install.json") }
    func loadDone() -> Set<String> {
        guard let d = try? Data(contentsOf: stateURL), let a = try? JSONDecoder().decode([String: [String]].self, from: d) else { return [] }
        return Set(a["done"] ?? [])
    }
    func markDone(_ id: String) {
        var s = loadDone(); s.insert(id)
        try? JSONEncoder().encode(["done": Array(s)]).write(to: stateURL)
    }
    func path(_ rel: String) -> String { base.appendingPathComponent(rel).path }

    // ---- process helper: streams stdout+stderr to logs/<step>.log and the progress callback ----
    @discardableResult
    func run(_ step: String, _ exe: String, _ args: [String], cwd: String? = nil, env: [String: String] = [:],
             lineHandler: ((String) -> Void)? = nil) throws -> Int32 {
        if cancelled { throw InstallError.msg("Cancelled") }
        try fm.createDirectory(at: base.appendingPathComponent("logs"), withIntermediateDirectories: true)
        let logPath = path("logs/\(step).log")
        if !fm.fileExists(atPath: logPath) { fm.createFile(atPath: logPath, contents: nil) }
        let log = try FileHandle(forWritingTo: URL(fileURLWithPath: logPath)); log.seekToEndOfFile()
        log.write(Data("$ \(exe) \(args.joined(separator: " "))\n".utf8))
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe); p.arguments = args
        if let c = cwd { p.currentDirectoryURL = URL(fileURLWithPath: c) }
        var e = ProcessInfo.processInfo.environment
        for (k, v) in env { e[k] = v }
        p.environment = e
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        var buffer = Data()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            if d.isEmpty { return }
            log.write(d)
            buffer.append(d)
            while let nl = buffer.firstIndex(of: 10) {
                let line = String(decoding: buffer[buffer.startIndex..<nl], as: UTF8.self)
                buffer.removeSubrange(buffer.startIndex...nl)
                lineHandler?(line)
            }
        }
        current = p
        try p.run(); p.waitUntilExit()
        pipe.fileHandleForReading.readabilityHandler = nil
        try? log.close()
        current = nil
        if cancelled { throw InstallError.msg("Cancelled") }
        if p.terminationStatus != 0 {
            throw InstallError.msg("\(URL(fileURLWithPath: exe).lastPathComponent) failed (exit \(p.terminationStatus)); see \(logPath)")
        }
        return p.terminationStatus
    }

    static func sha256(_ url: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: url); defer { try? h.close() }
        var hasher = SHA256()
        while let d = try h.read(upToCount: 1 << 20), !d.isEmpty { hasher.update(data: d) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // ---- the install ----
    func install() throws {
        try fm.createDirectory(at: base, withIntermediateDirectories: true)
        let done = loadDone()
        let fns: [(String, () throws -> Void)] = [
            ("check", stepCheck), ("extract", stepExtract), ("runtime", stepRuntime), ("rebase", stepRebase),
            ("catalog", stepCatalog), ("build", stepBuild), ("finish", stepFinish),
        ]
        for (i, (id, fn)) in fns.enumerated() {
            if cancelled { throw InstallError.msg("Cancelled") }
            current = nil
            if id != "check" && id != "finish" && done.contains(id) { onProgress(i, 1, "already done"); continue }
            onProgress(i, nil, installSteps[i].title)
            try fn()
            if id != "check" { markDone(id) }
            onProgress(i, 1, "done")
        }
    }

    func needTool(_ n: String) throws -> String {
        guard let t = Installer.tool(n) else { throw InstallError.msg("The helper tool '\(n)' is missing from the app.") }
        return t
    }

    func stepCheck() throws {
        #if !arch(arm64)
        throw InstallError.msg("This needs a Mac with Apple Silicon.")
        #endif
        guard fm.fileExists(atPath: setupExe.path) else { throw InstallError.msg("The installer file was not found.") }
        let missing = Installer.missingParts(of: setupExe)
        if !missing.isEmpty { throw InstallError.msg("The GOG installer's data files are missing next to it: \(missing.joined(separator: ", ")). Download all parts of the installer into one folder.") }
        if !Installer.commandLineToolsInstalled() { throw InstallError.msg("The Xcode Command Line Tools are needed to build the game on this Mac. Install them (xcode-select --install) and try again.") }
        if Installer.sourceRoot() == nil { throw InstallError.msg("The app's build scripts were not found.") }
        if Installer.llvmLib == nil { throw InstallError.msg("The app's bundled LLVM library was not found.") }
        if Installer.sdl3Prefix == nil { throw InstallError.msg("The app's bundled SDL3 was not found.") }
        if Installer.dxvkDylib == nil { throw InstallError.msg("The app's bundled DXVK was not found.") }
        _ = try needTool("innoextract"); _ = try needTool("cabextract"); _ = try needTool("msiextract")
        // free space (only counted when a fresh install is needed)
        var probe = base
        while !fm.fileExists(atPath: probe.path) { probe.deleteLastPathComponent() }
        if !loadDone().contains("extract"),
           let free = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage,
           free < 15_000_000_000 {
            throw InstallError.msg("About 15 GB of free space is needed; \(free / 1_000_000_000) GB is free on that disk.")
        }
    }

    func stepExtract() throws {
        let inno = try needTool("innoextract")
        let out = path("pc")
        var n = 0
        try run("extract", inno, ["--progress=1", "-s", "-d", out, setupExe.path]) { line in
            n += 1; self.onProgress(1, nil, line.isEmpty ? "extracting" : String(line.suffix(80)))
        }
        let exe = URL(fileURLWithPath: out + "/Binaries/MirrorsEdge.exe")
        guard fm.fileExists(atPath: exe.path) else { throw InstallError.msg("The installer did not contain Binaries/MirrorsEdge.exe. Is this Mirror's Edge?") }
        onProgress(1, nil, "checking the game's version")
        if try Installer.sha256(exe) != gameExeSHA256 {
            throw InstallError.msg("This is not the GOG version 1.01 of Mirror's Edge (the game executable differs). Only that build is supported.")
        }
        let bak = out + "/Binaries/PhysXCore.bak", dll = out + "/Binaries/PhysXCore.dll"
        if fm.fileExists(atPath: bak) && !fm.fileExists(atPath: dll) { try fm.copyItem(atPath: bak, toPath: dll) }
    }

    func stepRuntime() throws {
        let cab = try needTool("cabextract"), msi = try needTool("msiextract")
        let tmp = path("tmp-vcredist"), deps = path("pc-deps")
        try? fm.removeItem(atPath: tmp)
        try fm.createDirectory(atPath: tmp + "/u", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: deps, withIntermediateDirectories: true)
        let redist = path("pc/__redist/MSVC2005/vcredist_x86.exe")
        guard fm.fileExists(atPath: redist) else { throw InstallError.msg("The Visual C++ 2005 redistributable is not in the installer.") }
        try run("runtime", cab, ["-q", "-d", tmp, redist])
        let inner = try fm.contentsOfDirectory(atPath: tmp).first { $0.uppercased().hasPrefix("VCREDI") && $0.uppercased().hasSuffix(".EXE") }
        guard let innerExe = inner else { throw InstallError.msg("Unexpected layout inside vcredist_x86.exe.") }
        try run("runtime", cab, ["-q", "-d", tmp + "/u", tmp + "/" + innerExe])
        try run("runtime", msi, ["-C", tmp + "/u/msi", tmp + "/u/vcredist.msi"])
        for (name, want) in crtSHA256 {
            let src = tmp + "/u/msi/Windows/system32/" + name, dst = deps + "/" + name
            guard fm.fileExists(atPath: src) else { throw InstallError.msg("\(name) is missing from the redistributable.") }
            if fm.fileExists(atPath: dst) { try fm.removeItem(atPath: dst) }
            try fm.copyItem(atPath: src, toPath: dst)
            if try Installer.sha256(URL(fileURLWithPath: dst)) != want { throw InstallError.msg("\(name) is not the expected version (8.0.50727.762).") }
        }
        try? fm.removeItem(atPath: tmp)
    }

    func py(_ step: String, _ script: String, _ args: [String], lineHandler: ((String) -> Void)? = nil) throws {
        guard let src = Installer.sourceRoot() else { throw InstallError.msg("The build scripts were not found.") }
        try run(step, Installer.python, ["-I", src.path + "/pc/tools/" + script] + args, lineHandler: lineHandler)
    }

    func stepRebase() throws {
        try fm.createDirectory(atPath: path("pc-deps/rebased"), withIntermediateDirectories: true)
        for (i, (m, addr)) in rebasedDLLs.enumerated() {
            onProgress(3, Double(i) / Double(rebasedDLLs.count), m)
            try py("rebase", "rebase_pe.py", [path("pc/Binaries/\(m).dll"), path("pc-deps/rebased/\(m).dll"), addr])
        }
    }

    func stepCatalog() throws {
        guard let llvm = Installer.llvmLib else { throw InstallError.msg("LLVM missing") }
        var jobs: [(String, String, String)] = [   // (label, binary, output dir)
            ("MirrorsEdge.exe", path("pc/Binaries/MirrorsEdge.exe"), path("pc-recomp")),
            ("msvcr80", path("pc-deps/msvcr80.dll"), path("pc-recomp/msvcr80")),
            ("msvcp80", path("pc-deps/msvcp80.dll"), path("pc-recomp/msvcp80")),
        ]
        for (m, _) in rebasedDLLs { jobs.append((m, path("pc-deps/rebased/\(m).dll"), path("pc-recomp/\(m.lowercased())"))) }
        let lock = NSLock(); var finished = 0; var failure: Error?
        let q = OperationQueue(); q.maxConcurrentOperationCount = max(2, ProcessInfo.processInfo.activeProcessorCount / 2)
        for (label, bin, out) in jobs {
            if fm.fileExists(atPath: out + "/catalog.json") && label != "MirrorsEdge.exe" { lock.lock(); finished += 1; lock.unlock(); continue }
            q.addOperation {
                lock.lock(); let skip = failure != nil || self.cancelled; lock.unlock()
                if skip { return }
                do { try self.py("catalog", "catalog.py", [bin, "--llvm", llvm, "--out", out]) }
                catch { lock.lock(); failure = failure ?? error; lock.unlock(); return }
                lock.lock(); finished += 1; let f = Double(finished) / Double(jobs.count); lock.unlock()
                self.onProgress(4, f, "\(label) done")
            }
        }
        q.waitUntilAllOperationsAreFinished()
        if let f = failure { throw f }
    }

    func stepBuild() throws {
        guard let src = Installer.sourceRoot(), let llvm = Installer.llvmLib, let sdl = Installer.sdl3Prefix else { throw InstallError.msg("build inputs missing") }
        let args = ["-I", src.path + "/pc/tools/build_pc_runtime.py", "--native", "--libllvm", llvm, "--sdl3", sdl,
                    "--sdl3-bundle", "--base", base.path, "--work", path("pc-recomp")]
        try run("build", Installer.python, args, cwd: src.path) { line in
            // "compiled 150/248 (137 s)"
            if line.hasPrefix("compiled "), let frac = line.split(separator: " ").dropFirst().first?.split(separator: "/"),
               frac.count == 2, let a = Double(frac[0]), let b = Double(frac[1]), b > 0 {
                self.onProgress(5, 0.1 + 0.9 * a / b, line)
            } else if !line.isEmpty { self.onProgress(5, nil, String(line.suffix(80))) }
        }
        guard fm.fileExists(atPath: path("pc-recomp/bin-macos/me_pc")) else { throw InstallError.msg("The build finished without a game binary; see \(path("logs/build.log")).") }
    }

    func stepFinish() throws {
        try fm.createDirectory(atPath: path("dxvk/bin"), withIntermediateDirectories: true)
        try fm.createDirectory(atPath: path("pc-user"), withIntermediateDirectories: true)
        try fm.createDirectory(atPath: path("cache"), withIntermediateDirectories: true)
        if let d = Installer.dxvkDylib {
            let dst = path("dxvk/bin/libdxvk_d3d9.0.dylib")
            if fm.fileExists(atPath: dst) { try fm.removeItem(atPath: dst) }
            try fm.copyItem(atPath: d, toPath: dst)
        }
        var s = Store.load()
        s.meBinary = path("pc-recomp/bin-macos/me_pc"); s.gameDir = path("pc/Binaries"); s.depsDir = path("pc-deps")
        s.userDir = path("pc-user"); s.dxvkLib = path("dxvk/bin/libdxvk_d3d9.0.dylib")
        Store.save(s)
    }
}

/// `MELauncher --install <setup.exe> [--base <dir>]`: headless install for testing.
func headlessInstall(args: [String]) -> Int32 {
    guard let i = args.firstIndex(of: "--install"), i + 1 < args.count else { print("usage: --install <setup.exe> [--base dir]"); return 2 }
    var base = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("MirrorsEdge")
    if let b = args.firstIndex(of: "--base"), b + 1 < args.count { base = URL(fileURLWithPath: args[b + 1]) }
    let inst = Installer(setupExe: URL(fileURLWithPath: args[i + 1]), base: base)
    inst.onProgress = { idx, frac, msg in
        let pct = frac.map { String(format: "%3.0f%%", $0 * 100) } ?? "  ..."
        print("[\(idx + 1)/\(installSteps.count)] \(installSteps[idx].title) \(pct) \(msg)")
        fflush(stdout)
    }
    do { try inst.install(); print("installed into \(base.path)"); return 0 }
    catch { print("install failed: \(error)"); return 1 }
}
