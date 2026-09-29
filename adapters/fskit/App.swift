// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI

@main
struct MachlinBtrfsApp: App {
    var body: some Scene {
        WindowGroup("Machlin btrfs") {
            VStack(alignment: .leading, spacing: 16) {
                Text("Machlin btrfs").font(.largeTitle)
                Text("Read btrfs disks on macOS.").font(.title2)
                Text("Enable Machlin btrfs in System Settings → General → Login Items & Extensions → File System Extensions.")
                Text("This development build supports clean btrfs volumes in read-only mode.")
                    .foregroundStyle(.secondary)
            }
            .padding(32)
            .frame(width: 520)
        }
        .windowResizability(.contentSize)
    }
}
