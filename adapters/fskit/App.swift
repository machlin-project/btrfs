// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI
import ServiceManagement

@MainActor
private final class ServiceModel: ObservableObject {
    @Published var deviceService = "Checking…"
    @Published var status = ""

    func refresh() {
        DispatchQueue.global(qos: .userInitiated).async {
            let value = DeviceService.status()
            DispatchQueue.main.async { self.deviceService = value }
        }
    }

    func enableWriting() {
        do {
            try DeviceService.register()
            status = "Complete disk service setup, then remount the volume to enable writing."
        } catch {
            status = error.localizedDescription
        }
    }
}

struct MachlinBtrfsApp: App {
    @StateObject private var model = ServiceModel()

    var body: some Scene {
        WindowGroup("Machlin btrfs") {
            VStack(alignment: .leading, spacing: 16) {
                Text("Machlin btrfs").font(.largeTitle)
                Text("Read and write btrfs disks on macOS.").font(.title2)
                Text("Enable Machlin btrfs in System Settings → General → Login Items & Extensions → By Category → File System Extensions.")
                HStack {
                    Button("Open extension settings…") { SMAppService.openSystemSettingsLoginItems() }
                    Button("Enable disk writing…") { model.enableWriting() }
                    Text("Disk service: \(model.deviceService)").foregroundStyle(.secondary)
                }
                Text(model.status).foregroundStyle(.secondary)
                Text("Writing requires the approved disk service and writable media; without them volumes mount read-only.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(32)
            .frame(width: 560)
            .onAppear { model.refresh() }
        }
        .windowResizability(.contentSize)
    }
}

@main
enum MachlinBtrfsEntry {
    static func main() {
        let arguments = CommandLine.arguments
        if let index = arguments.firstIndex(of: "--control") {
            ControlCommand.run(Array(arguments[(index + 1)...]))
        }
        MachlinBtrfsApp.main()
    }
}
