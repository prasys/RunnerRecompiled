import AppKit
import Metal
import MetalFX
import QuartzCore

final class ViewController: NSViewController {
    var device: MTLDevice!
    var queue: MTLCommandQueue!
    var input: MTLTexture!
    var output: MTLTexture!
    var scaler: MTLFXSpatialScaler!
    let metalLayer = CAMetalLayer()
    var frame = 0

    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 1280, height: 832))
        view.wantsLayer = true
        device = MTLCreateSystemDefaultDevice()!
        queue = device.makeCommandQueue()!
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = false
        metalLayer.drawableSize = CGSize(width: 1280, height: 832)
        view.layer = metalLayer
        let desc = MTLFXSpatialScalerDescriptor()
        desc.inputWidth = 640; desc.inputHeight = 416
        desc.outputWidth = 1280; desc.outputHeight = 832
        desc.colorTextureFormat = .bgra8Unorm
        desc.outputTextureFormat = .bgra8Unorm
        desc.colorProcessingMode = .perceptual
        guard MTLFXSpatialScalerDescriptor.supportsDevice(device), let s = desc.makeSpatialScaler(device: device) else { fatalError("MetalFX spatial scaler unavailable") }
        scaler = s
        input = texture(640, 416)
        output = texture(1280, 832)
        loadPPM()
        Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.present() }
    }
    func texture(_ w: Int, _ h: Int) -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        return device.makeTexture(descriptor: d)!
    }
    func loadPPM() {
        guard CommandLine.arguments.count > 1, let data = try? Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])) else { return }
        let b = [UInt8](data); guard b.starts(with: Array("P6".utf8)) else { fatalError("expected P6 PPM") }
        var p = 2
        func tok() -> String { while p < b.count && b[p] <= 32 { p += 1 }; let s=p; while p < b.count && b[p] > 32 { p += 1 }; return String(bytes:b[s..<p], encoding:.ascii)! }
        let w=Int(tok())!, h=Int(tok())!; _=tok(); while p < b.count && b[p] <= 32 { p += 1 }
        guard w == 640 && h == 416 else { print("PPM is \(w)x\(h), expected 640x416"); return }
        let rgb=Array(b[p...]); var bgra=[UInt8](repeating:255,count:w*h*4)
        for i in 0..<(w*h) { bgra[i*4]=rgb[i*3+2]; bgra[i*4+1]=rgb[i*3+1]; bgra[i*4+2]=rgb[i*3] }
        input.replace(region:MTLRegionMake2D(0,0,w,h),mipmapLevel:0,withBytes:bgra,bytesPerRow:w*4)
    }
    func present() {
        guard let drawable=metalLayer.nextDrawable(), let cb=queue.makeCommandBuffer() else { return }
        scaler.colorTexture=input; scaler.outputTexture=output
        scaler.encode(commandBuffer:cb)
        let blit=cb.makeBlitCommandEncoder()!
        blit.copy(from:output,sourceSlice:0,sourceLevel:0,sourceOrigin:MTLOrigin(x:0,y:0,z:0),sourceSize:MTLSize(width:1280,height:832,depth:1),to:drawable.texture,destinationSlice:0,destinationLevel:0,destinationOrigin:MTLOrigin(x:0,y:0,z:0)); blit.endEncoding()
        cb.present(drawable); cb.commit(); frame += 1
        if frame == 120 { print("presented 120 MetalFX frames"); NSApp.terminate(nil) }
    }
}
let app=NSApplication.shared; app.setActivationPolicy(.regular)
let win=NSWindow(contentRect:NSRect(x:0,y:0,width:1280,height:832),styleMask:[.titled,.closable],backing:.buffered,defer:false)
win.title="MetalFX spatial scaler spike"; win.contentViewController=ViewController(); win.makeKeyAndOrderFront(nil); app.activate(ignoringOtherApps:true); app.run()
