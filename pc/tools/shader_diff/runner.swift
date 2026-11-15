// Metal runner of the shader differential test. Reads jobs.jsonl (build_jobs.py), compiles each side's MSL with
// fast math off, runs it on the given inputs and writes the raw float results next to the job:
//   pixel shaders:  a triangle over an 8x8 viewport per input set, RGBA32F colour targets + Depth32F
//   vertex shaders: a compute kernel that calls the vertex function for 16 vertices per input set
// usage: runner <jobs.jsonl> <textures.bin> [first [count]]
import Foundation
import Metal

struct Buf: Decodable { let index: Int; let offs: [Int] }
struct Tex: Decodable { let index: Int; let kind: String; let reg: Int }
struct Side: Decodable {
    let msl: String, bin: String, out: String
    let io: [Int]
    let buffers: [Buf]
    let textures: [Tex]
    let samplers: [Int]
    let colors: [Int]?
    let depth: Bool?
}
struct Job: Decodable { let name: String; let stage: String; let sides: [String: Side] }

let SETS = 4, W = 8, H = 8, VSN = 16, VSSLOTS = 40
let args = CommandLine.arguments
guard args.count >= 3, let dev = MTLCreateSystemDefaultDevice(), let queue = dev.makeCommandQueue() else {
    FileHandle.standardError.write("usage: runner <jobs.jsonl> <textures.bin> [first [count]]\n".data(using: .utf8)!)
    exit(2)
}

// ---- shared textures: 16 registers x (2D 16x16, cube 8x8, 3D 8x8x8), RGBA16F, all mips
let texData = try! Data(contentsOf: URL(fileURLWithPath: args[2]))
var texPos = 0
var textures: [String: [MTLTexture]] = [:]
for (kind, size) in [("2d", 16), ("cube", 8), ("3d", 8)] {
    var list: [MTLTexture] = []
    for _ in 0..<16 {
        let d = MTLTextureDescriptor()
        d.pixelFormat = .rgba16Float
        d.width = size; d.height = size
        d.textureType = kind == "2d" ? .type2D : kind == "cube" ? .typeCube : .type3D
        if kind == "3d" { d.depth = size }
        d.mipmapLevelCount = Int(log2(Double(size))) + 1
        d.usage = .shaderRead
        d.storageMode = .shared
        let t = dev.makeTexture(descriptor: d)!
        var s = size, level = 0
        while s >= 1 {
            let slices = kind == "cube" ? 6 : 1
            let dep = kind == "3d" ? s : 1
            for sl in 0..<slices {
                let n = s * s * dep * 8
                texData.withUnsafeBytes { p in
                    t.replace(region: MTLRegionMake3D(0, 0, 0, s, s, dep), mipmapLevel: level, slice: sl,
                              withBytes: p.baseAddress! + texPos, bytesPerRow: s * 8, bytesPerImage: s * s * 8)
                }
                texPos += n
            }
            s /= 2; level += 1
        }
        list.append(t)
    }
    textures[kind] = list
}
let sd = MTLSamplerDescriptor()
sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = .linear
// Experiments: SD_NOMIP=1 samples level 0 only, SD_NEAREST=1 also turns filtering off.
let env = ProcessInfo.processInfo.environment
if env["SD_NOMIP"] != nil || env["SD_NEAREST"] != nil { sd.mipFilter = .notMipmapped }
if env["SD_NEAREST"] != nil { sd.minFilter = .nearest; sd.magFilter = .nearest }
sd.sAddressMode = .repeat; sd.tAddressMode = .repeat; sd.rAddressMode = .repeat
let sampler = dev.makeSamplerState(descriptor: sd)!

// ---- render targets (pixel shaders): one 8x8 viewport per input set, stacked
func target(_ f: MTLPixelFormat) -> MTLTexture {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: W, height: H * SETS, mipmapped: false)
    d.usage = [.renderTarget, .shaderRead]
    d.storageMode = .shared
    return dev.makeTexture(descriptor: d)!
}
let colorT = (0..<4).map { _ in target(.rgba32Float) }
let depthT = target(.depth32Float)
let dsd = MTLDepthStencilDescriptor()
dsd.depthCompareFunction = .always
dsd.isDepthWriteEnabled = true
let depthState = dev.makeDepthStencilState(descriptor: dsd)!
let opts = MTLCompileOptions()
opts.mathMode = .safe
opts.languageVersion = .version3_0

func fail(_ s: Side, _ msg: String) {
    try? msg.write(toFile: s.out + ".err", atomically: true, encoding: .utf8)
}

func runPS(_ s: Side, _ lib: MTLLibrary, _ data: MTLBuffer) throws -> Data {
    let pd = MTLRenderPipelineDescriptor()
    pd.vertexFunction = lib.makeFunction(name: "sd_vs")
    pd.fragmentFunction = lib.makeFunction(name: "main0")
    let colors = s.colors ?? []
    for c in colors where c < 4 { pd.colorAttachments[c].pixelFormat = .rgba32Float }
    pd.depthAttachmentPixelFormat = .depth32Float
    let pso = try dev.makeRenderPipelineState(descriptor: pd)
    let rp = MTLRenderPassDescriptor()
    for c in colors where c < 4 {
        rp.colorAttachments[c].texture = colorT[c]
        rp.colorAttachments[c].loadAction = .clear
        rp.colorAttachments[c].storeAction = .store
        rp.colorAttachments[c].clearColor = MTLClearColor(red: -7777, green: -7777, blue: -7777, alpha: -7777)
    }
    rp.depthAttachment.texture = depthT
    rp.depthAttachment.loadAction = .clear
    rp.depthAttachment.storeAction = .store
    rp.depthAttachment.clearDepth = 1.0
    let cb = queue.makeCommandBuffer()!
    let enc = cb.makeRenderCommandEncoder(descriptor: rp)!
    enc.setRenderPipelineState(pso)
    enc.setDepthStencilState(depthState)
    enc.setCullMode(.none)
    for t in s.textures { enc.setFragmentTexture(textures[t.kind]![t.reg], index: t.index) }
    for i in s.samplers { enc.setFragmentSamplerState(sampler, index: i) }
    for set in 0..<SETS {
        enc.setViewport(MTLViewport(originX: 0, originY: Double(set * H), width: Double(W), height: Double(H), znear: 0, zfar: 1))
        enc.setScissorRect(MTLScissorRect(x: 0, y: set * H, width: W, height: H))
        enc.setVertexBuffer(data, offset: s.io[set], index: 0)
        for b in s.buffers { enc.setFragmentBuffer(data, offset: b.offs[set], index: b.index) }
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    }
    enc.endEncoding()
    cb.commit()
    cb.waitUntilCompleted()
    if let e = cb.error { throw e }
    // colours in order of `colors`, then depth as one float per pixel
    var out = Data()
    let n = W * H * SETS
    for c in colors where c < 4 {
        var px = [Float](repeating: 0, count: n * 4)
        colorT[c].getBytes(&px, bytesPerRow: W * 16, from: MTLRegionMake2D(0, 0, W, H * SETS), mipmapLevel: 0)
        px.withUnsafeBufferPointer { out.append(Data(buffer: $0)) }
    }
    var dp = [Float](repeating: 0, count: n)
    depthT.getBytes(&dp, bytesPerRow: W * 4, from: MTLRegionMake2D(0, 0, W, H * SETS), mipmapLevel: 0)
    dp.withUnsafeBufferPointer { out.append(Data(buffer: $0)) }
    return out
}

func runVS(_ s: Side, _ lib: MTLLibrary, _ data: MTLBuffer) throws -> Data {
    guard let fn = lib.makeFunction(name: "sd_wrap") else { throw NSError(domain: "sd", code: 1) }
    let pso = try dev.makeComputePipelineState(function: fn)
    let per = VSN * VSSLOTS * 16
    let outBuf = dev.makeBuffer(length: per * SETS, options: .storageModeShared)!
    let cb = queue.makeCommandBuffer()!
    let enc = cb.makeComputeCommandEncoder()!
    enc.setComputePipelineState(pso)
    for t in s.textures { enc.setTexture(textures[t.kind]![t.reg], index: t.index) }
    for i in s.samplers { enc.setSamplerState(sampler, index: i) }
    for set in 0..<SETS {
        enc.setBuffer(data, offset: s.io[set], index: 29)
        enc.setBuffer(outBuf, offset: per * set, index: 30)
        for b in s.buffers { enc.setBuffer(data, offset: b.offs[set], index: b.index) }
        enc.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                 threadsPerThreadgroup: MTLSize(width: VSN, height: 1, depth: 1))
    }
    enc.endEncoding()
    cb.commit()
    cb.waitUntilCompleted()
    if let e = cb.error { throw e }
    return Data(bytes: outBuf.contents(), count: per * SETS)
}

let lines = try! String(contentsOfFile: args[1], encoding: .utf8).split(separator: "\n")
let first = args.count > 3 ? Int(args[3])! : 0
let count = args.count > 4 ? Int(args[4])! : lines.count
var done = 0, failed = 0
let dec = JSONDecoder()
for line in lines.dropFirst(first).prefix(count) {
    autoreleasepool {
        guard let job = try? dec.decode(Job.self, from: line.data(using: .utf8)!) else { failed += 1; return }
        for (_, s) in job.sides {
            try? FileManager.default.removeItem(atPath: s.out + ".err")
            try? FileManager.default.removeItem(atPath: s.out)
            do {
                let src = try String(contentsOfFile: s.msl, encoding: .utf8)
                let lib = try dev.makeLibrary(source: src, options: opts)
                let bin = try Data(contentsOf: URL(fileURLWithPath: s.bin))
                let data = bin.withUnsafeBytes { dev.makeBuffer(bytes: $0.baseAddress!, length: bin.count, options: .storageModeShared)! }
                let out = job.stage == "ps" ? try runPS(s, lib, data) : try runVS(s, lib, data)
                try out.write(to: URL(fileURLWithPath: s.out))
            } catch {
                failed += 1
                fail(s, "\(error)")
            }
        }
        done += 1
    }
}
print("jobs \(done), failed sides \(failed)")
