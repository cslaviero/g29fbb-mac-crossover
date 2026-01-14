// Sources/g29ffb/main.swift

import Foundation
@preconcurrency import IOKit.hid

// MARK: - Utilities

func cfNumToInt(_ v: CFTypeRef?) -> Int? {
    guard let v else { return nil }
    if CFGetTypeID(v) == CFNumberGetTypeID() {
        var n: Int32 = 0
        if CFNumberGetValue((v as! CFNumber), .sInt32Type, &n) {
            return Int(n)
        }
    }
    return nil
}

func cfStr(_ v: CFTypeRef?) -> String? {
    guard let v else { return nil }
    if CFGetTypeID(v) == CFStringGetTypeID() {
        return (v as! CFString) as String
    }
    return nil
}

func hex(_ data: Data, max: Int = 64) -> String {
    let slice = data.prefix(max)
    return slice.map { String(format: "%02X", $0) }.joined(separator: " ")
}

func ceilDiv(_ a: Int, _ b: Int) -> Int { (a + b - 1) / b }

// MARK: - HID Report Descriptor (minimal parser for Output report sizes)

enum HIDItemType: UInt8 { case main = 0, global = 1, local = 2, reserved = 3 }

struct HIDParseState {
    var reportID: UInt8 = 0
    var reportSizeBits: Int = 0
    var reportCount: Int = 0
    // Accumulated sizes in bits for OUTPUT reports, per report ID
    var outputBits: [UInt8: Int] = [:]
}

/// Minimal parser:
/// - Tracks Global: Report ID (0x85), Report Size (0x75), Report Count (0x95)
/// - On Main: Output (tag 0x09 / prefix 0x91), adds size = reportSize * reportCount to current reportID
///
/// This is not a full HID parser, but is typically enough to compute report sizes.
func parseOutputReportSizes(from desc: Data) -> [UInt8: Int] {
    var st = HIDParseState()
    var i = 0
    let bytes = [UInt8](desc)

    while i < bytes.count {
        let prefix = bytes[i]
        i += 1

        if prefix == 0xFE {
            // Long item: 0xFE, size, longTag, data...
            if i + 2 > bytes.count { break }
            let size = Int(bytes[i]); i += 1
            _ = bytes[i]; i += 1 // longTag
            i += size
            continue
        }

        let sizeCode = Int(prefix & 0x03)
        let dataSize: Int
        switch sizeCode {
        case 0: dataSize = 0
        case 1: dataSize = 1
        case 2: dataSize = 2
        case 3: dataSize = 4
        default: dataSize = 0
        }

        let type = HIDItemType(rawValue: (prefix >> 2) & 0x03) ?? .reserved
        let tag = UInt8((prefix >> 4) & 0x0F)

        var value: UInt32 = 0
        if dataSize > 0 {
            if i + dataSize > bytes.count { break }
            // little-endian assemble
            for b in 0..<dataSize {
                value |= UInt32(bytes[i + b]) << UInt32(8 * b)
            }
            i += dataSize
        }

        switch type {
        case .global:
            // Global items tags (per HID spec):
            // 0x7: Report Size (0x75)
            // 0x9: Report Count (0x95)
            // 0x8: Report ID (0x85)
            if tag == 0x7 { st.reportSizeBits = Int(value) }
            else if tag == 0x9 { st.reportCount = Int(value) }
            else if tag == 0x8 { st.reportID = UInt8(value & 0xFF) }
        case .main:
            // Main Output item has tag 0x9 (prefix 0x91)
            if tag == 0x9 {
                let bits = st.reportSizeBits * st.reportCount
                if bits > 0 {
                    st.outputBits[st.reportID, default: 0] += bits
                }
            }
        default:
            break
        }
    }

    // Convert bits to bytes (ceil)
    var out: [UInt8: Int] = [:]
    for (rid, bits) in st.outputBits {
        out[rid] = ceilDiv(bits, 8)
    }
    return out
}

// MARK: - Logitech “Classic” protocol payload builders (7 bytes)
// These builders generate the 7-byte *protocol payload* described in the Logitech Classic FFB PDF.
// We keep the payload at exactly 7 bytes (byte0..byte6). Report ID is passed separately to IOHIDDeviceSetReport.

@inline(__always)
func makeCmdByte(slotMask: UInt8, cmd: UInt8) -> UInt8 {
    // byte0: [F3 F2 F1 F0 | CMD(4 bits)]
    return ((slotMask & 0x0F) << 4) | (cmd & 0x0F)
}

func payloadStopForce(slotMask: UInt8) -> [UInt8] {
    // CMD 0x03: Stop Force
    return [
        makeCmdByte(slotMask: slotMask, cmd: 0x03),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    ]
}

func payloadDefaultSpringOff(slotMask: UInt8) -> [UInt8] {
    // CMD 0x05: Default Spring Off
    return [
        makeCmdByte(slotMask: slotMask, cmd: 0x05),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    ]
}

func payloadFixedTimeLoop(enable2ms: Bool) -> [UInt8] {
    // CMD 0x0D: Fixed Time Loop
    // byte1: 0x01 = update forces every 2ms, 0x00 = disabled
    return [
        0x0D,
        enable2ms ? 0x01 : 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00
    ]
}

func payloadDownloadPlayConstantForce(
    slotMask: UInt8,
    f0: UInt8,
    f1: UInt8 = 0x80,
    f2: UInt8 = 0x80,
    f3: UInt8 = 0x80
) -> [UInt8] {
    // CMD 0x01: Download and Play Force
    // byte1: FORCE_TYPE = 0x00 (Constant Force)
    // bytes 2..5: force levels for F0..F3
    // byte6: unused/padding
    return [
        makeCmdByte(slotMask: slotMask, cmd: 0x01),
        0x00,
        f0, f1, f2, f3,
        0x00
    ]
}

func sleepMs(_ ms: Int) {
    Thread.sleep(forTimeInterval: Double(ms) / 1000.0)
}

// MARK: - HID device discovery

struct HIDDeviceInfo {
    let device: IOHIDDevice
    let vendorID: Int
    let productID: Int
    let product: String
}

func findLogitechG29Devices() -> [HIDDeviceInfo] {
    let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
    IOHIDManagerSetDeviceMatching(mgr, [
        kIOHIDVendorIDKey as String: 0x046D // Logitech vendor ID
    ] as CFDictionary)

    IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
    defer { IOHIDManagerClose(mgr, IOOptionBits(kIOHIDOptionsTypeNone)) }

    guard let set = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> else { return [] }

    var out: [HIDDeviceInfo] = []
    for d in set {
        let vid = cfNumToInt(IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString)) ?? -1
        let pid = cfNumToInt(IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString)) ?? -1
        let name = cfStr(IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString)) ?? "(unknown)"

        // Filter on "G29" substring to avoid other Logitech devices
        if name.localizedCaseInsensitiveContains("G29") || name.localizedCaseInsensitiveContains("Driving Force") {
            out.append(HIDDeviceInfo(device: d, vendorID: vid, productID: pid, product: name))
        }
    }
    return out
}

func getReportDescriptor(_ d: IOHIDDevice) -> Data? {
    guard let cf = IOHIDDeviceGetProperty(d, kIOHIDReportDescriptorKey as CFString) else { return nil }
    if CFGetTypeID(cf) == CFDataGetTypeID() {
        return (cf as! Data)
    }
    return nil
}

func elementInt(_ e: IOHIDElement, _ key: CFString) -> Int? {
    return cfNumToInt(IOHIDElementGetProperty(e, key))
}

func dumpPIDOutputElements(_ d: IOHIDDevice) -> [UInt8: Int] {
    let match: [String: Any] = [
        kIOHIDElementTypeKey as String: kIOHIDElementTypeOutput
    ]
    guard let elems = IOHIDDeviceCopyMatchingElements(
        d,
        match as CFDictionary,
        IOOptionBits(kIOHIDOptionsTypeNone)
    ) as? [IOHIDElement] else {
        print("PID output elements: (no output elements found)")
        return [:]
    }

    let pidPage = 0x0F
    var totals: [UInt8: Int] = [:]
    var count = 0

    print("PID output elements (usage page 0x0F):")
    for e in elems {
        guard let usagePage = elementInt(e, kIOHIDElementUsagePageKey as CFString),
              usagePage == pidPage else { continue }

        count += 1
        let usage = elementInt(e, kIOHIDElementUsageKey as CFString) ?? -1
        let reportID = elementInt(e, kIOHIDElementReportIDKey as CFString) ?? 0
        let reportSizeBits = elementInt(e, kIOHIDElementReportSizeKey as CFString) ?? 0
        let reportCount = elementInt(e, kIOHIDElementReportCountKey as CFString) ?? 0
        let bits = reportSizeBits * reportCount
        let bytes = ceilDiv(bits, 8)

        let rid = UInt8(reportID & 0xFF)
        totals[rid, default: 0] += bytes

        print(
            "  rid=0x\(String(format: "%02X", rid)) usage=0x\(String(format: "%02X", usage)) " +
            "sizeBits=\(reportSizeBits) count=\(reportCount) bytes=\(bytes)"
        )
    }

    if count == 0 {
        print("  (no PID output elements found)")
    } else {
        print("PID output totals by reportID (sum of element sizes):")
        for (rid, bytes) in totals.sorted(by: { $0.key < $1.key }) {
            print("  rid=0x\(String(format: "%02X", rid)) -> \(bytes) bytes")
        }
    }

    return totals
}

// MARK: - Send output report

func openDevice(_ d: IOHIDDevice) -> Bool {
    let r = IOHIDDeviceOpen(d, IOOptionBits(kIOHIDOptionsTypeNone))
    return r == kIOReturnSuccess
}

func closeDevice(_ d: IOHIDDevice) {
    _ = IOHIDDeviceClose(d, IOOptionBits(kIOHIDOptionsTypeNone))
}

func sendOutputReport(_ d: IOHIDDevice, reportID: UInt8, bytes: [UInt8]) -> IOReturn {
    var buf = bytes
    return IOHIDDeviceSetReport(
        d,
        kIOHIDReportTypeOutput,
        CFIndex(reportID),
        &buf,
        buf.count
    )
}

func sendClassicPayload(
    _ d: IOHIDDevice,
    reportID: UInt8,
    payload7: [UInt8],
    label: String
) {
    precondition(payload7.count == 7, "Classic payload must be exactly 7 bytes")
    let r = sendOutputReport(d, reportID: reportID, bytes: payload7)
    if r == kIOReturnSuccess {
        print("[OK]   \(label) -> \(payload7.map{String(format:"%02X",$0)}.joined(separator:" "))")
    } else {
        print("[FAIL] \(label) IOHIDDeviceSetReport: 0x\(String(format:"%08X", r))")
    }
}

func runScriptedTestSequence(_ d: IOHIDDevice, reportID: UInt8) {
    // We use all force slots (0xF) for stop/spring-off to be safe.
    // For constant force, we drive F0 only (slotMask=0x1) so it’s easy to reason about.

    print("\n=== Scripted Logitech Classic FFB test sequence (reportID=0x\(String(format:"%02X", reportID))) ===")
    print("Tip: keep hands light on the wheel. This test will apply strong constant force briefly.")

    sendClassicPayload(d, reportID: reportID, payload7: payloadStopForce(slotMask: 0x0F), label: "Stop all")
    sleepMs(100)

    sendClassicPayload(d, reportID: reportID, payload7: payloadDefaultSpringOff(slotMask: 0x0F), label: "Default spring off")
    sleepMs(100)

    sendClassicPayload(d, reportID: reportID, payload7: payloadFixedTimeLoop(enable2ms: true), label: "Fixed loop ON (2ms)")
    sleepMs(100)

    // Strong one direction
    sendClassicPayload(
        d,
        reportID: reportID,
        payload7: payloadDownloadPlayConstantForce(slotMask: 0x01, f0: 0x40),
        label: "Constant force F0=0x40 (strong)"
    )
    sleepMs(1000)

    // Strong opposite direction
    sendClassicPayload(
        d,
        reportID: reportID,
        payload7: payloadDownloadPlayConstantForce(slotMask: 0x01, f0: 0xC0),
        label: "Constant force F0=0xC0 (strong opposite)"
    )
    sleepMs(1000)

    // Neutral
    sendClassicPayload(
        d,
        reportID: reportID,
        payload7: payloadDownloadPlayConstantForce(slotMask: 0x01, f0: 0x80),
        label: "Constant force F0=0x80 (neutral)"
    )
    sleepMs(250)

    sendClassicPayload(d, reportID: reportID, payload7: payloadStopForce(slotMask: 0x0F), label: "Stop all")
    sleepMs(100)

    print("=== End scripted test ===\n")
}

// MARK: - UDP host (daemon)

final class UDPServer {
    private let fd: Int32
    private let source: DispatchSourceRead
    private let queue: DispatchQueue
    private let onMessage: (String) -> Void

    init(port: UInt16, onMessage: @escaping (String) -> Void) throws {
        self.queue = DispatchQueue(label: "g29ffb.udp")
        self.onMessage = onMessage

        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw NSError(domain: "socket", code: 1) }
        self.fd = fd

        var yes: Int32 = 1
        _ = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout.size(ofValue: yes)))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_LOOPBACK.bigEndian)

        let bindResult = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else {
            close(fd)
            throw NSError(domain: "bind", code: 2)
        }

        self.source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        self.source.setEventHandler { [weak self] in
            self?.readOnce()
        }
        self.source.setCancelHandler { [fd] in
            close(fd)
        }
        self.source.resume()
    }

    private func readOnce() {
        var buf = [UInt8](repeating: 0, count: 1024)
        var addr = sockaddr_in()
        var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        let bufCount = buf.count
        let n = buf.withUnsafeMutableBytes { raw -> Int in
            guard let base = raw.baseAddress else { return -1 }
            return withUnsafeMutablePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    recvfrom(fd, base, bufCount, 0, sa, &addrLen)
                }
            }
        }

        guard n > 0 else { return }
        let data = Data(buf[0..<n])
        if let msg = String(data: data, encoding: .utf8) {
            onMessage(msg)
        }
    }
}

struct HostConfig {
    var port: UInt16 = 21999
    var rateHz: Int = 200
    var watchdogMs: Int = 250
    var maxForce: Int = 100
    var deviceIndex: Int? = nil
    var reportID: UInt8 = 0x00
}

final class FFBHost {
    private let wheel: IOHIDDevice
    private let reportID: UInt8
    private let outputSize: Int
    private let maxForce: Int
    private let watchdogMs: Int
    private let keepAliveMs: UInt64

    private let queue = DispatchQueue(label: "g29ffb.host")
    private var server: UDPServer?
    private var timer: DispatchSourceTimer?

    private var desiredForce: Int8 = 0
    private var activeForce: Int8 = 0
    private var lastUpdateMs: UInt64 = 0
    private var lastSendMs: UInt64 = 0
    private var loopEnabled = false
    private var lastLogMs: UInt64 = 0

    init(wheel: IOHIDDevice, reportID: UInt8, outputSize: Int, config: HostConfig) {
        self.wheel = wheel
        self.reportID = reportID
        self.outputSize = outputSize
        self.maxForce = max(1, min(127, config.maxForce))
        self.watchdogMs = max(50, config.watchdogMs)
        self.keepAliveMs = UInt64(max(10, 1000 / max(50, config.rateHz)))
    }

    func start(port: UInt16, rateHz: Int) throws {
        let _ = sendInit()
        self.server = try UDPServer(port: port) { [weak self] msg in
            self?.handleMessage(msg)
        }
        let intervalMs = max(1, 1000 / max(50, rateHz))
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(intervalMs))
        timer.setEventHandler { [weak self] in
            self?.tick()
        }
        self.timer = timer
        timer.resume()
        print("FFB host running on 127.0.0.1:\(port) rate=\(rateHz)Hz watchdog=\(watchdogMs)ms maxForce=\(maxForce)")
        dispatchMain()
    }

    private func nowMs() -> UInt64 {
        UInt64(Date().timeIntervalSince1970 * 1000.0)
    }

    private func clampForce(_ v: Int) -> Int8 {
        let clamped = max(-maxForce, min(maxForce, v))
        return Int8(clamped)
    }

    private func handleMessage(_ msg: String) {
        let trimmed = msg.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return }
        let upper = trimmed.uppercased()
        if upper == "STOP" {
            desiredForce = 0
            lastUpdateMs = nowMs()
            logIncoming("STOP")
            return
        }
        if upper.hasPrefix("CONST") {
            let parts = trimmed.split(whereSeparator: { $0 == " " || $0 == "\t" })
            if parts.count >= 2, let v = Int(parts[1]) {
                desiredForce = clampForce(v)
                lastUpdateMs = nowMs()
                logIncoming("CONST \(desiredForce)")
            }
        }
    }

    private func logIncoming(_ line: String) {
        let now = nowMs()
        if now - lastLogMs < 200 { return }
        lastLogMs = now
        print("[daemon] \(line)")
    }

    private func tick() {
        let now = nowMs()
        if lastUpdateMs > 0, now - lastUpdateMs > UInt64(watchdogMs) {
            if activeForce != 0 {
                sendStop()
                activeForce = 0
                lastSendMs = now
            }
            return
        }

        if desiredForce != activeForce || now - lastSendMs >= keepAliveMs {
            if desiredForce == 0 {
                sendStop()
            } else {
                sendConstant(desiredForce)
            }
            activeForce = desiredForce
            lastSendMs = now
        }
    }

    private func sendInit() {
        sendPayload(payloadStopForce(slotMask: 0x0F))
        sendPayload(payloadDefaultSpringOff(slotMask: 0x0F))
        sendPayload(payloadFixedTimeLoop(enable2ms: true))
        loopEnabled = true
    }

    private func sendStop() {
        sendPayload(payloadStopForce(slotMask: 0x0F))
    }

    private func sendConstant(_ force: Int8) {
        if !loopEnabled {
            sendPayload(payloadFixedTimeLoop(enable2ms: true))
            loopEnabled = true
        }
        let f0 = UInt8(Int(force) + 0x80)
        sendPayload(payloadDownloadPlayConstantForce(slotMask: 0x01, f0: f0))
    }

    private func sendPayload(_ payload7: [UInt8]) {
        var buf = payload7
        if outputSize > buf.count {
            buf.append(contentsOf: Array(repeating: 0x00, count: outputSize - buf.count))
        }
        var out = buf
        _ = IOHIDDeviceSetReport(
            wheel,
            kIOHIDReportTypeOutput,
            CFIndex(reportID),
            &out,
            out.count
        )
    }
}

// MARK: - CLI

func readLineInt(prompt: String) -> Int? {
    print(prompt, terminator: "")
    guard let s = readLine(), !s.isEmpty else { return nil }
    return Int(s.trimmingCharacters(in: .whitespacesAndNewlines))
}

func readLineHexByte(prompt: String) -> UInt8? {
    print(prompt, terminator: "")
    guard let s = readLine(), !s.isEmpty else { return nil }
    let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
    if t.hasPrefix("0x") || t.hasPrefix("0X") {
        return UInt8(t.dropFirst(2), radix: 16)
    } else if let v = UInt8(t, radix: 16) {
        return v
    } else if let v = UInt8(t) {
        return v
    }
    return nil
}

// MARK: - Main

func parseHostConfig(_ args: [String]) -> HostConfig {
    var cfg = HostConfig()
    var i = 0
    while i < args.count {
        let a = args[i]
        switch a {
        case "--port":
            if i + 1 < args.count, let p = UInt16(args[i + 1]) { cfg.port = p; i += 1 }
        case "--rate":
            if i + 1 < args.count, let r = Int(args[i + 1]) { cfg.rateHz = r; i += 1 }
        case "--watchdog":
            if i + 1 < args.count, let w = Int(args[i + 1]) { cfg.watchdogMs = w; i += 1 }
        case "--max":
            if i + 1 < args.count, let m = Int(args[i + 1]) { cfg.maxForce = m; i += 1 }
        case "--device":
            if i + 1 < args.count, let d = Int(args[i + 1]) { cfg.deviceIndex = d; i += 1 }
        case "--report-id":
            if i + 1 < args.count {
                let t = args[i + 1]
                let v: UInt8?
                if t.hasPrefix("0x") || t.hasPrefix("0X") {
                    v = UInt8(t.dropFirst(2), radix: 16)
                } else {
                    v = UInt8(t, radix: 16) ?? UInt8(t)
                }
                if let v { cfg.reportID = v; i += 1 }
            }
        default:
            break
        }
        i += 1
    }
    return cfg
}

func runInteractive() {
    let devices = findLogitechG29Devices()
    if devices.isEmpty {
        print("No Logitech G29-like HID devices found via IOHIDManager.")
        exit(1)
    }

    print("Found \(devices.count) candidate device(s):")
    for (i, di) in devices.enumerated() {
        print("[\(i)] \(di.product) vid=0x\(String(format:"%04X", di.vendorID)) pid=0x\(String(format:"%04X", di.productID))")
    }

    let idx = readLineInt(prompt: "Select device index: ") ?? 0
    guard idx >= 0 && idx < devices.count else {
        print("Invalid index.")
        exit(1)
    }

    let dev = devices[idx].device
    guard openDevice(dev) else {
        print("Failed to open IOHIDDevice. Try running with sudo.")
        exit(1)
    }
    defer { closeDevice(dev) }

    guard let desc = getReportDescriptor(dev) else {
        print("Could not read report descriptor.")
        exit(1)
    }

    print("Report descriptor bytes (first 64): \(hex(desc, max: 64))")
    let outSizes = parseOutputReportSizes(from: desc)
    if outSizes.isEmpty {
        print("No output reports detected in descriptor (or parser didn't find them).")
    } else {
        print("Output report sizes (ReportID -> bytes):")
        for (rid, sz) in outSizes.sorted(by: { $0.key < $1.key }) {
            print("  rid=0x\(String(format:"%02X", rid)) -> \(sz) bytes")
        }
    }

    if let maxOut = cfNumToInt(IOHIDDeviceGetProperty(dev, kIOHIDMaxOutputReportSizeKey as CFString)) {
        print("Max output report size: \(maxOut) bytes")
    }

    let pidTotals = dumpPIDOutputElements(dev)

    print("""
    Next: scripted Logitech Classic FFB test sequence.
    - Enter the reportID you want to use.
    - The tool will run a short sequence: stop, spring-off, fixed-loop ON, constant force one way, the other way, neutral, stop.
    - After that, you can optionally do manual single-packet sends.

    Notes:
    - This tool sends the 7-byte Logitech Classic *protocol payload* as defined in the PDF.
    - Some HID interfaces accept different reportIDs; if you feel nothing, try reportID 0 then 1, and try the other device index.
    """)

    var defaultRID: UInt8 = 0x00
    if pidTotals.count == 1, let onlyRID = pidTotals.keys.first {
        defaultRID = onlyRID
    }
    let ridForScript = readLineHexByte(prompt: "ReportID for scripted test (hex, see PID list above): ") ?? defaultRID
    runScriptedTestSequence(dev, reportID: ridForScript)

    print("Enter manual mode? (y/N): ", terminator: "")
    let manual = (readLine() ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    if manual == "y" || manual == "yes" {
        while true {
            guard let rid = readLineHexByte(prompt: "Manual ReportID (hex), empty to quit: ") else {
                break
            }
            print("Manual commands: 1=const(F0), 3=stop-all, 5=spring-off, d=fixedloop-on, x=fixedloop-off")
            print("Command: ", terminator: "")
            guard let cmdStr = readLine(), !cmdStr.isEmpty else { break }
            let c = cmdStr.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()

            let payload7: [UInt8]
            let label: String

            switch c {
            case "1":
                guard let force = readLineHexByte(prompt: "F0 force level (hex). Try 40/C0/80: ") else { continue }
                payload7 = payloadDownloadPlayConstantForce(slotMask: 0x01, f0: force)
                label = "Constant force F0=0x\(String(format:"%02X", force))"
            case "3":
                payload7 = payloadStopForce(slotMask: 0x0F)
                label = "Stop all"
            case "5":
                payload7 = payloadDefaultSpringOff(slotMask: 0x0F)
                label = "Default spring off"
            case "d":
                payload7 = payloadFixedTimeLoop(enable2ms: true)
                label = "Fixed loop ON (2ms)"
            case "x":
                payload7 = payloadFixedTimeLoop(enable2ms: false)
                label = "Fixed loop OFF"
            default:
                print("Unknown command")
                continue
            }

            sendClassicPayload(dev, reportID: rid, payload7: payload7, label: label)
        }
    }

    print("Done.")
}

func runDaemon(args: [String]) {
    let cfg = parseHostConfig(args)
    let devices = findLogitechG29Devices()
    if devices.isEmpty {
        print("No Logitech G29-like HID devices found via IOHIDManager.")
        exit(1)
    }

    let idx = cfg.deviceIndex ?? 0
    guard idx >= 0 && idx < devices.count else {
        print("Invalid device index.")
        exit(1)
    }

    let dev = devices[idx].device
    guard openDevice(dev) else {
        print("Failed to open IOHIDDevice. Try running with sudo.")
        exit(1)
    }

    let outputSize = cfNumToInt(IOHIDDeviceGetProperty(dev, kIOHIDMaxOutputReportSizeKey as CFString)) ?? 16
    print("Using device index \(idx) reportID=0x\(String(format: "%02X", cfg.reportID)) outputSize=\(outputSize) bytes")

    let host = FFBHost(wheel: dev, reportID: cfg.reportID, outputSize: outputSize, config: cfg)
    do {
        try host.start(port: cfg.port, rateHz: cfg.rateHz)
    } catch {
        print("Failed to start UDP host: \(error)")
        exit(1)
    }
}

func run() {
    let args = Array(CommandLine.arguments.dropFirst())
    if args.contains("--daemon") {
        runDaemon(args: args)
    } else {
        runInteractive()
    }
}

run()
