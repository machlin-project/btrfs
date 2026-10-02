// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI
import ServiceManagement
import Darwin

// ServiceManagement requires an unsandboxed installer for an unsandboxed daemon.
// This separate utility only manages that service; the control app stays sandboxed.
private enum DiskService {
    static let service = SMAppService.daemon(plistName: "group.org.machlin.btrfs.device-barrier.plist")

    static var status: String {
        switch service.status {
        case .enabled: return "enabled"
        case .requiresApproval: return "requiresApproval"
        case .notRegistered: return "notRegistered"
        case .notFound: return "notFound"
        @unknown default: return "unknown"
        }
    }

    static func register() throws {
        do {
            try service.register()
        } catch {
            // Registration can report EPERM while awaiting normal system approval.
            guard service.status == .requiresApproval else { throw error }
        }
    }

    static func unregister() async throws {
        var mounts: UnsafeMutablePointer<statfs>?
        let count = getmntinfo(&mounts, MNT_NOWAIT)
        guard count > 0, let mounts else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(EIO))
        }
        for index in 0..<Int(count) {
            let type = withUnsafeBytes(of: mounts[index].f_fstypename) {
                String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self)
            }
            guard type != "machlinbtrfs" else {
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(EBUSY), userInfo: [
                    NSLocalizedDescriptionKey: "Unmount all Machlin btrfs volumes before updating or removing the disk service."
                ])
            }
        }
        // Wait for the documented completion boundary before re-registration.
        try await service.unregister()
    }

    static func refresh() async throws {
        // Registering an already enabled job does not replace its running code.
        // Use the public lifecycle after a bundle update, with volumes unmounted.
        if service.status != .notRegistered {
            try await unregister()
        }
        // macOS can still return EPERM right after the documented asynchronous
        // completion. Retry only that transient, still-unregistered state, with a
        // short finite budget; approval and signing errors keep their handling.
        let delays: [UInt64] = [0, 250_000_000, 500_000_000, 1_000_000_000, 2_000_000_000]
        for (index, delay) in delays.enumerated() {
            if delay != 0 { try await Task.sleep(nanoseconds: delay) }
            do {
                try register()
                return
            } catch {
                let failure = error as NSError
                guard failure.domain == SMAppServiceErrorDomain,
                      failure.code == Int(EPERM), service.status == .notRegistered,
                      index + 1 < delays.count else { throw error }
            }
        }
    }
}

private struct SetupView: View {
    @State private var status = DiskService.status
    @State private var updating = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Enable btrfs disk writing").font(.title)
            Text("The disk service synchronizes device caches so btrfs can commit writes safely. Approve the service in System Settings, then remount the volume.")
            Text(status).foregroundStyle(.secondary)
            HStack {
                Button("Enable service") {
                    do {
                        try DiskService.register()
                        status = DiskService.status
                        if DiskService.service.status == .requiresApproval {
                            SMAppService.openSystemSettingsLoginItems()
                        }
                    } catch {
                        status = error.localizedDescription
                    }
                }
                Button("Open System Settings") { SMAppService.openSystemSettingsLoginItems() }
                Button("Refresh") { status = DiskService.status }
            }
            Button("Update service after installing a new version") {
                updating = true
                Task {
                    defer { updating = false }
                    do {
                        try await DiskService.refresh()
                        status = DiskService.status
                        if DiskService.service.status == .requiresApproval {
                            SMAppService.openSystemSettingsLoginItems()
                        }
                    } catch {
                        status = error.localizedDescription
                    }
                }
            }
        }
        .disabled(updating)
        .padding(24)
        .frame(width: 500)
    }
}

private struct SetupApp: App {
    var body: some Scene {
        WindowGroup("Machlin btrfs Disk Service") { SetupView() }
    }
}

@main
enum DeviceSetup {
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        if arguments.isEmpty {
            SetupApp.main()
            return
        }
        Task { await command(arguments) }
        dispatchMain()
    }

    private static func command(_ arguments: [String]) async {
        do {
            guard arguments.count == 1 else {
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL))
            }
            switch arguments[0] {
            case "--register": try DiskService.register()
            case "--unregister": try await DiskService.unregister()
            case "--refresh": try await DiskService.refresh()
            case "--status": break
            default: throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL))
            }
            let data = try JSONSerialization.data(withJSONObject: ["status": DiskService.status])
            FileHandle.standardOutput.write(data + Data([10]))
        } catch {
            let value = error as NSError
            let data = try? JSONSerialization.data(withJSONObject: [
                "error": ["domain": value.domain, "code": value.code, "message": value.localizedDescription]
            ])
            if let data { FileHandle.standardError.write(data + Data([10])) }
            exit(EXIT_FAILURE)
        }
        exit(EXIT_SUCCESS)
    }
}
