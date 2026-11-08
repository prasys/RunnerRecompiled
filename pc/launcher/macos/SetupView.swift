import SwiftUI
import AppKit
import UniformTypeIdentifiers

/// First-run wizard: "do you have the game?" -> drop the GOG installer -> where it is stored -> (Command Line Tools)
/// -> install with progress -> Play. The work itself is in Installer.swift.
enum SetupPhase { case welcome, pick, location, tools, installing, failed, done }
enum StepState { case waiting, running, done }

@MainActor final class SetupModel: ObservableObject {
    @Published var phase: SetupPhase = .welcome
    @Published var setupExe: URL?
    @Published var pickError = ""
    @Published var base = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("MirrorsEdge")
    @Published var states = [StepState](repeating: .waiting, count: installSteps.count)
    @Published var fraction: Double?
    @Published var message = ""
    @Published var error = ""
    @Published var elapsed = 0
    private var installer: Installer?
    private var timer: Timer?
    private var started = Date()

    var freeGB: Int {
        var p = base
        while !FileManager.default.fileExists(atPath: p.path), p.path != "/" { p.deleteLastPathComponent() }
        let v = try? p.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage
        return Int((v ?? 0) / 1_000_000_000)
    }

    func choose(_ url: URL) {
        pickError = ""
        guard url.pathExtension.lowercased() == "exe" else { pickError = "That is not the installer. Pick the setup_mirrors_edge…exe file from GOG."; return }
        let missing = Installer.missingParts(of: url)
        guard missing.isEmpty else { pickError = "The installer's data files are missing next to it (\(missing.joined(separator: ", "))). Put all parts of the GOG download in one folder."; return }
        setupExe = url
        phase = .location
    }

    func openPanel() {
        let p = NSOpenPanel(); p.allowedContentTypes = [UTType(filenameExtension: "exe") ?? .data]
        p.message = "Choose your GOG installer for Mirror's Edge (setup_mirrors_edge_…exe)"
        if p.runModal() == .OK, let u = p.url { choose(u) }
    }

    func chooseFolder() {
        let p = NSOpenPanel(); p.canChooseDirectories = true; p.canChooseFiles = false; p.canCreateDirectories = true
        p.message = "Choose where the game should be stored (about 15 GB)"
        if p.runModal() == .OK, let u = p.url {
            base = u.lastPathComponent == "MirrorsEdge" ? u : u.appendingPathComponent("MirrorsEdge")
        }
    }

    func next() {   // from "location": the tools check comes first
        phase = Installer.commandLineToolsInstalled() ? .installing : .tools
        if phase == .installing { start() }
    }

    func installTools() {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select"); p.arguments = ["--install"]
        try? p.run()
    }

    func recheckTools() { if Installer.commandLineToolsInstalled() { phase = .installing; start() } }

    func start() {
        guard let exe = setupExe else { return }
        phase = .installing
        states = [StepState](repeating: .waiting, count: installSteps.count)
        error = ""; elapsed = 0; started = Date(); fraction = nil
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.elapsed = Int(Date().timeIntervalSince(self?.started ?? Date())) }
        }
        let inst = Installer(setupExe: exe, base: base)
        installer = inst
        inst.onProgress = { [weak self] idx, frac, msg in
            DispatchQueue.main.async {
                guard let s = self else { return }
                for i in 0..<s.states.count { s.states[i] = i < idx ? .done : (i == idx ? .running : s.states[i]) }
                if frac == 1 { s.states[idx] = .done }
                s.fraction = frac == 1 ? nil : (frac ?? s.fraction)
                if idx != s.lastIdx { s.fraction = nil; s.lastIdx = idx }
                s.message = msg
            }
        }
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                try inst.install()
                DispatchQueue.main.async { self.timer?.invalidate(); self.phase = .done }
            } catch {
                DispatchQueue.main.async {
                    self.timer?.invalidate()
                    self.error = "\(error)"
                    self.phase = .failed
                }
            }
        }
    }
    var lastIdx = -1

    func cancel() { installer?.cancel() }
}

struct SetupView: View {
    @StateObject var m = SetupModel()
    var onFinished: () -> Void
    @State private var dropping = false

    var body: some View {
        VStack(spacing: 0) {
            ZStack(alignment: .bottomLeading) {
                SkylineArt().frame(height: 120).clipped()
                Text("Mirror's Edge for Mac").font(.system(size: 26, weight: .bold)).foregroundStyle(Color.black.opacity(0.75)).padding(16)
            }
            Group {
                switch m.phase {
                case .welcome: welcome
                case .pick: pick
                case .location: location
                case .tools: tools
                case .installing: installing
                case .failed: failed
                case .done: done
                }
            }
            .padding(24).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .frame(width: 680, height: 560)
    }

    var welcome: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Do you have the game?").font(.title2.bold())
            Text("This app runs your own copy of Mirror's Edge natively on your Mac. It needs the GOG installer, version 1.01 (the one named setup_mirrors_edge_1.01_jpfix_(41238).exe, with its .bin files). Nothing from the game is included with this app; it builds the Mac version from your files and keeps everything in one folder.")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("I have the installer") { m.phase = .pick }.keyboardShortcut(.defaultAction)
                Button("Where do I get it?") { NSWorkspace.shared.open(URL(string: "https://www.gog.com/en/game/mirrors_edge")!) }
            }
        }
    }

    var pick: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Drag the installer here").font(.title2.bold())
            ZStack {
                RoundedRectangle(cornerRadius: 14).strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [8]))
                    .foregroundStyle(dropping ? Color.accentColor : Color.secondary)
                VStack(spacing: 8) {
                    Image(systemName: "arrow.down.doc").font(.system(size: 36))
                    Text("setup_mirrors_edge_1.01_jpfix_(41238).exe").font(.callout)
                    Text("Keep its -1.bin, -2.bin and -3.bin files in the same folder.").font(.caption).foregroundStyle(.secondary)
                }
            }
            .frame(height: 170)
            .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
                guard let p = providers.first else { return false }
                _ = p.loadObject(ofClass: URL.self) { url, _ in if let url { DispatchQueue.main.async { m.choose(url) } } }
                return true
            }
            if !m.pickError.isEmpty { Text(m.pickError).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true) }
            HStack {
                Button("Choose file…") { m.openPanel() }
                Button("Back") { m.phase = .welcome }
            }
        }
    }

    var location: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Where should it be stored?").font(.title2.bold())
            Text("Installer: \(m.setupExe?.lastPathComponent ?? "")").font(.caption).foregroundStyle(.secondary)
            HStack {
                Text(m.base.path).font(.system(.body, design: .monospaced)).lineLimit(1).truncationMode(.middle)
                Spacer()
                Button("Change…") { m.chooseFolder() }
            }
            .padding(10).background(RoundedRectangle(cornerRadius: 8).fill(.quaternary))
            Text("The extracted game, the Mac build, your settings and saves all go in this folder. It needs about 15 GB; \(m.freeGB) GB is free here.")
                .fixedSize(horizontal: false, vertical: true)
            if m.freeGB < 15 { Text("There is not enough free space on this disk.").foregroundStyle(.red) }
            Text("To delete the game later, remove that folder.").font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("Install") { m.next() }.keyboardShortcut(.defaultAction).disabled(m.freeGB < 15)
                Button("Back") { m.phase = .pick }
            }
        }
    }

    var tools: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("One more thing: Apple's developer tools").font(.title2.bold())
            Text("The game is built on your Mac from your files, and that needs Apple's free Command Line Tools (about 1 GB). Click Install, follow Apple's prompt, then come back and press Check again.")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Install Command Line Tools") { m.installTools() }
                Button("Check again") { m.recheckTools() }.keyboardShortcut(.defaultAction)
                Button("Back") { m.phase = .location }
            }
        }
    }

    var installing: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Installing").font(.title2.bold())
            ForEach(Array(installSteps.enumerated()), id: \.offset) { i, st in
                HStack {
                    switch m.states[i] {
                    case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                    case .running: ProgressView().controlSize(.small)
                    case .waiting: Image(systemName: "circle").foregroundStyle(.secondary)
                    }
                    Text(st.title).foregroundStyle(m.states[i] == .waiting ? .secondary : .primary)
                }
            }
            if let f = m.fraction { ProgressView(value: f) } else { ProgressView().progressViewStyle(.linear) }
            Text(m.message).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            HStack {
                Text("Elapsed \(m.elapsed / 60):\(String(format: "%02d", m.elapsed % 60)). This takes 15-30 minutes; you can leave it running.")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Cancel") { m.cancel() }
            }
        }
    }

    var failed: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("The install stopped").font(.title2.bold())
            ScrollView { Text(m.error).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }.frame(maxHeight: 150)
            Text("Finished steps are kept, so Try again continues where it stopped.").font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("Try again") { m.start() }.keyboardShortcut(.defaultAction)
                Button("Show logs") { NSWorkspace.shared.open(m.base.appendingPathComponent("logs")) }
                Button("Start over") { m.phase = .welcome }
            }
        }
    }

    var done: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Ready to play").font(.title2.bold())
            Text("The game is installed in \(m.base.path). The first start is silent for about 20 seconds, and level loads are slow the first time you visit them.")
                .fixedSize(horizontal: false, vertical: true)
            Button("Continue") { onFinished() }.keyboardShortcut(.defaultAction)
        }
    }
}

/// Shows the wizard until a game build exists, then the launcher.
struct RootView: View {
    @State var installed = installedAlready()
    var body: some View {
        if installed { ContentView() } else { SetupView { installed = true } }
    }
}

func installedAlready() -> Bool {
    let s = Store.load(), fm = FileManager.default
    return !s.meBinary.isEmpty && fm.fileExists(atPath: s.meBinary) && fm.fileExists(atPath: s.gameDir + "/MirrorsEdge.exe")
}
