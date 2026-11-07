import AppKit
import Metal
import MetalFX
import QuartzCore

final class ViewController: NSViewController {
    var device: MTLDevice!
    var queue: MTLCommandQueue!
    var input: MTLTexture!
    var metalFXOutput: MTLTexture!
    var bilinearOutput: MTLTexture!
    var scaler: MTLFXSpatialScaler!
    let metalLayer = CAMetalLayer()
    var rendered = false

    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 960, height: 600))
        view.wantsLayer = true
        device = MTLCreateSystemDefaultDevice()!
        queue = device.makeCommandQueue()!
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = false
        metalLayer.drawableSize = CGSize(width: 960, height: 600)
        view.layer = metalLayer
        let desc = MTLFXSpatialScalerDescriptor()
        desc.inputWidth = 960; desc.inputHeight = 600
        desc.outputWidth = 1920; desc.outputHeight = 1200
        desc.colorTextureFormat = .bgra8Unorm
        desc.outputTextureFormat = .bgra8Unorm
        desc.colorProcessingMode = .perceptual
        guard MTLFXSpatialScalerDescriptor.supportsDevice(device), let s = desc.makeSpatialScaler(device: device) else { fatalError("MetalFX spatial scaler unavailable") }
        scaler = s
        input = texture(960, 600, [.shaderRead, .shaderWrite])
        metalFXOutput = texture(1920, 1200, [.shaderRead, .shaderWrite])
        bilinearOutput = texture(1920, 1200, [.shaderRead, .shaderWrite])
        loadPPM(CommandLine.arguments[1])
        render()
    }
    func texture(_ w: Int, _ h: Int, _ usage: MTLTextureUsage) -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        d.usage = usage
        return device.makeTexture(descriptor: d)!
    }
    func loadPPM(_ path: String) {
        let data = try! Data(contentsOf: URL(fileURLWithPath: path)); let b = [UInt8](data)
        guard b.starts(with: Array("P6".utf8)) else { fatalError("expected binary P6 PPM") }
        var p = 2
        func token() -> String { while p < b.count && b[p] <= 32 { p += 1 }; let s=p; while p < b.count && b[p] > 32 { p += 1 }; return String(bytes:b[s..<p], encoding:.ascii)! }
        let w=Int(token())!, h=Int(token())!, maxv=Int(token())!
        guard w == 960 && h == 600 && maxv == 255 else { fatalError("expected 960x600 P6 PPM maxval 255; got \(w)x\(h) max \(maxv)") }
        if p < b.count && b[p] == 13 { p += 1 }; if p < b.count && b[p] == 10 { p += 1 } else if p < b.count && b[p] == 32 { p += 1 }
        let rgb=Array(b[p...]); guard rgb.count >= w*h*3 else { fatalError("truncated PPM") }
        var bgra=[UInt8](repeating:255,count:w*h*4)
        for i in 0..<(w*h) { bgra[i*4]=rgb[i*3+2]; bgra[i*4+1]=rgb[i*3+1]; bgra[i*4+2]=rgb[i*3] }
        input.replace(region:MTLRegionMake2D(0,0,w,h),mipmapLevel:0,withBytes:bgra,bytesPerRow:w*4)
    }
    func render() {
        guard let cb=queue.makeCommandBuffer() else { fatalError("command buffer") }
        scaler.colorTexture=input; scaler.outputTexture=metalFXOutput
        scaler.encode(commandBuffer:cb)
        cb.commit(); cb.waitUntilCompleted()
        guard cb.status == .completed else { fatalError("GPU work failed: \(String(describing: cb.error))") }
        let out=URL(fileURLWithPath:CommandLine.arguments[2],isDirectory:true)
        try! FileManager.default.createDirectory(at:out,withIntermediateDirectories:true)
        savePNG(metalFXOutput,out.appendingPathComponent("frame_900_metalfx_1920x1200.png"))
        saveBilinearPPM(CommandLine.arguments[1],out.appendingPathComponent("frame_900_bilinear_1920x1200.png"))
        print("Saved MetalFX and bilinear PNGs from \(CommandLine.arguments[1])")
        if let d=metalLayer.nextDrawable(),let display=queue.makeCommandBuffer() {
            let e=display.makeBlitCommandEncoder()!
            e.copy(from:metalFXOutput,sourceSlice:0,sourceLevel:0,sourceOrigin:MTLOrigin(x:0,y:0,z:0),sourceSize:MTLSize(width:1920,height:1200,depth:1),to:d.texture,destinationSlice:0,destinationLevel:0,destinationOrigin:MTLOrigin(x:0,y:0,z:0)); e.endEncoding(); display.present(d); display.commit()
        }
        rendered=true
        DispatchQueue.main.asyncAfter(deadline:.now()+1) { NSApp.terminate(nil) }
    }
    func saveBilinearPPM(_ path: String, _ url: URL) {
        let b=[UInt8](try! Data(contentsOf:URL(fileURLWithPath:path))); let base=15 // P6 960 600 255\n header
        guard b.count >= base+960*600*3 else { fatalError("short PPM for bilinear output") }
        var pixels=[UInt8](repeating:255,count:1920*1200*4)
        for y in 0..<1200 { let fy=(Double(y)+0.5)/2.0-0.5; let y0=max(0,min(599,Int(floor(fy)))); let y1=min(599,y0+1); let wy=max(0,min(1,fy-Double(y0)))
            for x in 0..<1920 { let fx=(Double(x)+0.5)/2.0-0.5; let x0=max(0,min(959,Int(floor(fx)))); let x1=min(959,x0+1); let wx=max(0,min(1,fx-Double(x0)))
                let oi=(y*1920+x)*4
                for c in 0..<3 { let a=Double(b[base+(y0*960+x0)*3+c]),bb=Double(b[base+(y0*960+x1)*3+c]),cc=Double(b[base+(y1*960+x0)*3+c]),d=Double(b[base+(y1*960+x1)*3+c]); let top=a*(1-wx)+bb*wx, bot=cc*(1-wx)+d*wx; pixels[oi+(2-c)]=UInt8(max(0,min(255,(top*(1-wy)+bot*wy).rounded()))) }
            }
        }
        let rep=NSBitmapImageRep(bitmapDataPlanes:nil,pixelsWide:1920,pixelsHigh:1200,bitsPerSample:8,samplesPerPixel:4,hasAlpha:true,isPlanar:false,colorSpaceName:.deviceRGB,bytesPerRow:1920*4,bitsPerPixel:32)!
        rep.bitmapData!.update(from:pixels,count:pixels.count)
        try! rep.representation(using:.png,properties:[:])!.write(to:url)
    }
    func savePNG(_ texture: MTLTexture, _ url: URL) {
        let w=texture.width,h=texture.height; var pixels=[UInt8](repeating:0,count:w*h*4)
        texture.getBytes(&pixels,bytesPerRow:w*4,from:MTLRegionMake2D(0,0,w,h),mipmapLevel:0)
        let rep=NSBitmapImageRep(bitmapDataPlanes:nil,pixelsWide:w,pixelsHigh:h,bitsPerSample:8,samplesPerPixel:4,hasAlpha:true,isPlanar:false,colorSpaceName:.deviceRGB,bytesPerRow:w*4,bitsPerPixel:32)!
        rep.bitmapData!.update(from:pixels,count:pixels.count)
        try! rep.representation(using:.png,properties:[:])!.write(to:url)
    }
}
let app=NSApplication.shared; app.setActivationPolicy(.regular)
let win=NSWindow(contentRect:NSRect(x:0,y:0,width:960,height:600),styleMask:[.titled,.closable],backing:.buffered,defer:false)
win.title="MetalFX spatial scaler comparison"; win.contentViewController=ViewController(); win.makeKeyAndOrderFront(nil); app.activate(ignoringOtherApps:true); app.run()
