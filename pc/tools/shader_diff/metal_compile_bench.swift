import Foundation
import Metal
// usage: pbench <msl dir> <pairs.txt> <threads> <salt or ""> [limit]
let a = CommandLine.arguments
let dir = a[1], threads = Int(a[3])!, salt = a[4]
let limit = a.count > 5 ? Int(a[5])! : Int.max
let dev = MTLCreateSystemDefaultDevice()!
let pairs = try! String(contentsOfFile: a[2]).split(separator: "\n").prefix(limit).map { $0.split(separator: " ").map(String.init) }
var libs = [String: MTLLibrary]()
let lock = NSLock()
var libErr = 0, pipeErr = 0, libNs: UInt64 = 0, pipeNs: UInt64 = 0
var firstErr = ""
func lib(_ name: String, _ ext: String) -> MTLLibrary? {
  lock.lock(); if let l = libs[name] { lock.unlock(); return l }; lock.unlock()
  var src = try! String(contentsOfFile: "\(dir)/\(name).\(ext).metal")
  if !salt.isEmpty { src += "\n// \(salt)\n" }
  let o = MTLCompileOptions(); o.fastMathEnabled = false
  let t = DispatchTime.now().uptimeNanoseconds
  let l = try? dev.makeLibrary(source: src, options: o)
  let d = DispatchTime.now().uptimeNanoseconds - t
  lock.lock(); libNs += d; if l == nil { libErr += 1 } else { libs[name] = l }; lock.unlock()
  return l
}
let t0 = DispatchTime.now().uptimeNanoseconds
DispatchQueue.concurrentPerform(iterations: threads) { w in
  var i = w
  while i < pairs.count {
    let p = pairs[i]; i += threads
    guard let vl = lib(p[0], "vert"), let fl = lib(p[1], "frag"),
          let vf = vl.makeFunction(name: "main0"), let ff = fl.makeFunction(name: "main0") else { continue }
    let d = MTLRenderPipelineDescriptor()
    d.vertexFunction = vf; d.fragmentFunction = ff
    for k in 0..<4 { d.colorAttachments[k].pixelFormat = .bgra8Unorm }
    d.depthAttachmentPixelFormat = .depth32Float
    let t = DispatchTime.now().uptimeNanoseconds
    do { _ = try dev.makeRenderPipelineState(descriptor: d) } catch {
      lock.lock(); pipeErr += 1; if firstErr.isEmpty { firstErr = "\(p) \(error)" }; lock.unlock()
    }
    let dt = DispatchTime.now().uptimeNanoseconds - t
    lock.lock(); pipeNs += dt; lock.unlock()
  }
}
let wall = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9
print("pairs \(pairs.count) threads \(threads) wall \(String(format: "%.1f", wall)) s | libs \(libs.count) libErr \(libErr) cpu-in-lib \(Double(libNs)/1e9) s | pipeErr \(pipeErr) cpu-in-pipe \(Double(pipeNs)/1e9) s")
if !firstErr.isEmpty { print(firstErr.prefix(400)) }
