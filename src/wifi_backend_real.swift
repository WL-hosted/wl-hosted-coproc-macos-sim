import CoreWLAN
import Foundation

// Real macOS WiFi backend for wlh-coproc-macos-sim.
// Exposed to the C veneer (wifi_backend_real.c) via @_cdecl, primitives only.
// All functions run on the simulator's wifi worker thread and may block.

typealias WlhRealBssCallback = @convention(c) (
    UnsafeMutableRawPointer?, // callback context
    UnsafePointer<UInt8>?, // ssid bytes
    Int, // ssid length
    UnsafePointer<UInt8>?, // bssid, 6 bytes
    UInt32, // WifiSecurity value (protocol wifi.proto)
    UInt32, // channel
    Int32 // rssi dBm
) -> Void

// WifiSecurity values from wl-hosted-protocol wifi.proto.
private enum WifiSecurity {
    static let unspecified: UInt32 = 0
    static let open: UInt32 = 1
    static let wep: UInt32 = 2
    static let wpaPsk: UInt32 = 3
    static let wpa2Psk: UInt32 = 4
    static let wpaWpa2Psk: UInt32 = 5
    static let wpa3Sae: UInt32 = 6
    static let wpa2Wpa3Psk: UInt32 = 7
    static let owe: UInt32 = 8
    static let wpa2Enterprise: UInt32 = 9
    static let wpa3Enterprise: UInt32 = 10
}

private struct BssReport {
    var ssid: Data
    var bssid: [UInt8]
    var security: UInt32
    var channel: UInt32
    var rssi: Int32

    func deliver(to callback: WlhRealBssCallback, context: UnsafeMutableRawPointer?) {
        ssid.withUnsafeBytes { ssidBuffer in
            bssid.withUnsafeBufferPointer { bssidBuffer in
                callback(
                    context,
                    ssidBuffer.baseAddress?.assumingMemoryBound(to: UInt8.self),
                    ssidBuffer.count,
                    bssidBuffer.baseAddress,
                    security,
                    channel,
                    rssi
                )
            }
        }
    }
}

private func parseBssid(_ string: String?) -> [UInt8]? {
    guard let string else { return nil }
    let bytes = string.split(separator: ":").compactMap { UInt8($0, radix: 16) }
    return bytes.count == 6 ? bytes : nil
}

private func securityValue(of network: CWNetwork) -> UInt32 {
    if network.supportsSecurity(.wpa3Enterprise) { return WifiSecurity.wpa3Enterprise }
    if network.supportsSecurity(.wpa3Personal) { return WifiSecurity.wpa3Sae }
    if network.supportsSecurity(.wpa3Transition) { return WifiSecurity.wpa2Wpa3Psk }
    if network.supportsSecurity(.wpa2Enterprise) { return WifiSecurity.wpa2Enterprise }
    if network.supportsSecurity(.wpaEnterpriseMixed) { return WifiSecurity.wpa2Enterprise }
    if network.supportsSecurity(.wpaEnterprise) { return WifiSecurity.wpa2Enterprise }
    if network.supportsSecurity(.OWE) { return WifiSecurity.owe }
    if network.supportsSecurity(.oweTransition) { return WifiSecurity.owe }
    if network.supportsSecurity(.wpa2Personal) { return WifiSecurity.wpa2Psk }
    if network.supportsSecurity(.wpaPersonalMixed) { return WifiSecurity.wpaWpa2Psk }
    if network.supportsSecurity(.wpaPersonal) { return WifiSecurity.wpaPsk }
    if network.supportsSecurity(.dynamicWEP) { return WifiSecurity.wep }
    if network.supportsSecurity(.WEP) { return WifiSecurity.wep }
    if network.supportsSecurity(CWSecurity.none) { return WifiSecurity.open }
    return WifiSecurity.unspecified
}

private func makeReport(
    of network: CWNetwork,
    fallbackSSID: Data? = nil,
    interface: CWInterface? = nil
) -> BssReport? {
    guard let ssid = network.ssidData, !ssid.isEmpty else {
        guard let fallbackSSID, !fallbackSSID.isEmpty else { return nil }
        return BssReport(
            ssid: fallbackSSID,
            bssid: parseBssid(network.bssid) ?? parseBssid(interface?.bssid()) ?? [0, 0, 0, 0, 0, 0],
            security: securityValue(of: network),
            channel: UInt32(network.wlanChannel?.channelNumber ?? 0),
            rssi: Int32(network.rssiValue)
        )
    }
    guard let bssid = parseBssid(network.bssid) else { return nil }
    return BssReport(
        ssid: ssid,
        bssid: bssid,
        security: securityValue(of: network),
        channel: UInt32(network.wlanChannel?.channelNumber ?? 0),
        rssi: Int32(network.rssiValue)
    )
}

private final class RealWiFiBackend {
    let interface: CWInterface?

    init() {
        interface = CWWiFiClient.shared().interface()
    }
}

private func backend(from handle: UnsafeMutableRawPointer?) -> RealWiFiBackend? {
    guard let handle else { return nil }
    return Unmanaged<RealWiFiBackend>.fromOpaque(handle).takeUnretainedValue()
}

@_cdecl("wlh_real_swift_create")
func wlhRealSwiftCreate() -> UnsafeMutableRawPointer? {
    Unmanaged.passRetained(RealWiFiBackend()).toOpaque()
}

@_cdecl("wlh_real_swift_destroy")
func wlhRealSwiftDestroy(_ handle: UnsafeMutableRawPointer?) {
    guard let handle else { return }
    Unmanaged<RealWiFiBackend>.fromOpaque(handle).release()
}

@_cdecl("wlh_real_swift_initialize")
func wlhRealSwiftInitialize(_ handle: UnsafeMutableRawPointer?) -> Int32 {
    guard let interface = backend(from: handle)?.interface else { return -1 }
    if interface.powerOn() { return 0 }
    do {
        try interface.setPower(true)
    } catch {
        return -1
    }
    return interface.powerOn() ? 0 : -1
}

@_cdecl("wlh_real_swift_scan")
func wlhRealSwiftScan(
    _ handle: UnsafeMutableRawPointer?,
    _ callback: WlhRealBssCallback?,
    _ context: UnsafeMutableRawPointer?
) -> Int32 {
    guard let interface = backend(from: handle)?.interface, let callback else { return -1 }
    let networks: Set<CWNetwork>
    do {
        networks = try interface.scanForNetworks(withName: nil)
    } catch {
        return -1
    }
    var count: Int32 = 0
    for network in networks {
        guard let report = makeReport(of: network) else { continue }
        report.deliver(to: callback, context: context)
        count += 1
    }
    return count
}

@_cdecl("wlh_real_swift_connect")
func wlhRealSwiftConnect(
    _ handle: UnsafeMutableRawPointer?,
    _ ssid: UnsafePointer<UInt8>?,
    _ ssidLength: Int,
    _ password: UnsafePointer<CChar>?,
    _ callback: WlhRealBssCallback?,
    _ context: UnsafeMutableRawPointer?
) -> Int32 {
    guard let interface = backend(from: handle)?.interface,
          let ssid, ssidLength > 0, ssidLength <= 32
    else { return -3 }
    let ssidData = Data(UnsafeBufferPointer(start: ssid, count: ssidLength))
    let networks: Set<CWNetwork>
    do {
        networks = try interface.scanForNetworks(withSSID: ssidData)
    } catch {
        return -2
    }
    guard let network = networks.first(where: { $0.ssidData == ssidData }) ?? networks.first
    else { return -2 }
    let credential = password.map { String(cString: $0) }.flatMap { $0.isEmpty ? nil : $0 }
    do {
        try interface.associate(to: network, password: credential)
    } catch {
        return -3
    }
    if let callback,
       let report = makeReport(of: network, fallbackSSID: ssidData, interface: interface)
    {
        report.deliver(to: callback, context: context)
    }
    return 0
}

@_cdecl("wlh_real_swift_disconnect")
func wlhRealSwiftDisconnect(_ handle: UnsafeMutableRawPointer?) -> Int32 {
    backend(from: handle)?.interface?.disassociate()
    return 0
}
