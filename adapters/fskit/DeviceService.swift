// SPDX-License-Identifier: BSD-3-Clause
import Foundation
import AppKit

enum DeviceService {
    static func status() -> String {
        BtrfsDeviceBarrier.isServiceAvailable() ? "enabled" : "unavailable"
    }

    static func register() throws {
        let setup = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/BtrfsDeviceSetup.app")
        guard NSWorkspace.shared.open(setup) else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(EIO), userInfo: [
                NSLocalizedDescriptionKey: "Could not open the disk service setup utility."
            ])
        }
    }
}
