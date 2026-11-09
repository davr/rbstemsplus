// Test tool, not part of the app. Boots a tart VM with a USB mass storage device (Apple's
// VZUSBMassStorageDevice, macOS 15 or later) backed by a raw image, which macOS in the VM sees as
// external, protocol USB: the app accepts it as a chosen location, unlike `tart run --disk` or a
// disk image, and macOS asks for access to it. It can be pulled and plugged in while the VM runs.
//
//   xcrun swiftc -swift-version 5 -O tests/vm-usb.swift -o vmusb
//   codesign -f -s - --entitlements <a plist with com.apple.security.virtualization> vmusb
//   truncate -s 2g usb.img; mkfifo ctl
//   ./vmusb ~/.tart/vms/<clone> <shared dir> usb.img ctl
//   echo attach > ctl; echo detach > ctl; echo stop > ctl
//
// Format the stick in the VM once it's attached (diskutil eraseDisk ExFAT NAME MBR diskN). The VM
// keeps its MAC address, so `tart ip <clone>` finds it.
// fifo commands: attach, detach, stop
import AppKit
import Virtualization

setvbuf(stdout, nil, _IOLBF, 0)
let a = CommandLine.arguments
guard a.count == 5 else { print("usage: vmusb vmdir shareddir usbimage fifo"); exit(2) }
let vmDir = URL(fileURLWithPath: a[1]), shared = URL(fileURLWithPath: a[2]), usbImage = URL(fileURLWithPath: a[3]), fifo = a[4]

struct Config: Decodable { let cpuCount: Int; let memorySize: UInt64; let ecid: String; let hardwareModel: String; let macAddress: String; let display: Display
    struct Display: Decodable { let width: Int; let height: Int } }
let cfg = try JSONDecoder().decode(Config.self, from: Data(contentsOf: vmDir.appendingPathComponent("config.json")))

let c = VZVirtualMachineConfiguration()
c.cpuCount = cfg.cpuCount
c.memorySize = cfg.memorySize
c.bootLoader = VZMacOSBootLoader()
let platform = VZMacPlatformConfiguration()
platform.hardwareModel = VZMacHardwareModel(dataRepresentation: Data(base64Encoded: cfg.hardwareModel)!)!
platform.machineIdentifier = VZMacMachineIdentifier(dataRepresentation: Data(base64Encoded: cfg.ecid)!)!
platform.auxiliaryStorage = VZMacAuxiliaryStorage(url: vmDir.appendingPathComponent("nvram.bin"))
c.platform = platform
let gfx = VZMacGraphicsDeviceConfiguration()
gfx.displays = [VZMacGraphicsDisplayConfiguration(widthInPixels: cfg.display.width, heightInPixels: cfg.display.height, pixelsPerInch: 72)]
c.graphicsDevices = [gfx]
let snd = VZVirtioSoundDeviceConfiguration()
let out = VZVirtioSoundDeviceOutputStreamConfiguration(); out.sink = VZHostAudioOutputStreamSink()
snd.streams = [out]
c.audioDevices = [snd]
c.keyboards = [VZMacKeyboardConfiguration()]
c.pointingDevices = [VZMacTrackpadConfiguration(), VZUSBScreenCoordinatePointingDeviceConfiguration()]
let net = VZVirtioNetworkDeviceConfiguration()
net.attachment = VZNATNetworkDeviceAttachment()
net.macAddress = VZMACAddress(string: cfg.macAddress)!
c.networkDevices = [net]
c.storageDevices = [VZVirtioBlockDeviceConfiguration(attachment: try VZDiskImageStorageDeviceAttachment(url: vmDir.appendingPathComponent("disk.img"), readOnly: false))]
c.entropyDevices = [VZVirtioEntropyDeviceConfiguration()]
c.memoryBalloonDevices = [VZVirtioTraditionalMemoryBalloonDeviceConfiguration()]
let fs = VZVirtioFileSystemDeviceConfiguration(tag: VZVirtioFileSystemDeviceConfiguration.macOSGuestAutomountTag)
fs.share = VZMultipleDirectoryShare(directories: ["xfer": VZSharedDirectory(url: shared, readOnly: false)])
c.directorySharingDevices = [fs]
c.usbControllers = [VZXHCIControllerConfiguration()]
try c.validate()

let vm = VZVirtualMachine(configuration: c)
var stick: VZUSBMassStorageDevice?

func attach() {
    guard stick == nil else { print("usb: already attached"); return }
    do {
        let att = try VZDiskImageStorageDeviceAttachment(url: usbImage, readOnly: false)
        let d = VZUSBMassStorageDevice(configuration: VZUSBMassStorageDeviceConfiguration(attachment: att))
        vm.usbControllers[0].attach(device: d) { e in
            if let e = e { print("usb: attach failed: \(e)") } else { stick = d; print("usb: attached") }
        }
    } catch { print("usb: attach failed: \(error)") }
}
func detach() {
    guard let d = stick else { print("usb: not attached"); return }
    vm.usbControllers[0].detach(device: d) { e in
        if let e = e { print("usb: detach failed: \(e)") } else { stick = nil; print("usb: detached") }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let view = VZVirtualMachineView()
view.virtualMachine = vm
view.capturesSystemKeys = true
view.automaticallyReconfiguresDisplay = true
let win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: cfg.display.width, height: cfg.display.height), styleMask: [.titled, .resizable, .miniaturizable], backing: .buffered, defer: false)
win.title = "\(vmDir.lastPathComponent) (with USB)"
win.contentView = view
win.center()
win.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)

class D: NSObject, VZVirtualMachineDelegate {
    func guestDidStop(_ vm: VZVirtualMachine) { print("vm: stopped"); exit(0) }
    func virtualMachine(_ vm: VZVirtualMachine, didStopWithError e: Error) { print("vm: stopped with error: \(e)"); exit(1) }
}
let delegate = D()
vm.delegate = delegate
vm.start { r in
    if case .failure(let e) = r { print("vm: start failed: \(e)"); exit(1) }
    print("vm: started")
}

Thread.detachNewThread {
    while true {
        guard let f = fopen(fifo, "r") else { sleep(1); continue }
        var buf = [CChar](repeating: 0, count: 64)
        while fgets(&buf, 64, f) != nil {
            let cmd = String(cString: buf).trimmingCharacters(in: .whitespacesAndNewlines)
            DispatchQueue.main.async {
                switch cmd {
                case "attach": attach()
                case "detach": detach()
                case "stop": print("vm: stopping"); vm.stop { _ in exit(0) }
                default: print("unknown: \(cmd)")
                }
            }
        }
        fclose(f)
    }
}
app.run()
