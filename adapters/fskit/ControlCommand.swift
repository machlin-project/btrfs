// SPDX-License-Identifier: BSD-3-Clause
import Foundation
import FSKit
import Darwin

/// Runs inside the signed app, so command-line checks use the app's own
/// sandbox, App Group and code-signing identity.
enum ControlCommand {
    private static let usage = """
    Usage: Machlin btrfs --control modules
           Machlin btrfs --control device-service [setup]
    """

    private static func writeJSON(_ value: Any, to handle: FileHandle = .standardOutput) throws {
        var data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
        data.append(0x0a)
        try handle.write(contentsOf: data)
    }

    private static func fail(_ error: Error) -> Never {
        let error = error as NSError
        try? writeJSON([
            "error": ["domain": error.domain, "code": error.code,
                      "message": error.localizedDescription]
        ], to: .standardError)
        exit(EXIT_FAILURE)
    }

    static func run(_ arguments: [String]) -> Never {
        guard let command = arguments.first else {
            FileHandle.standardError.write(Data((usage + "\n").utf8))
            exit(EX_USAGE)
        }
        do {
            let result: Any
            switch (command, arguments.count) {
            case ("device-service", 1):
                result = ["status": DeviceService.status()]
            case ("device-service", 2) where arguments[1] == "setup":
                try DeviceService.register()
                result = ["setupOpened": true]
            case ("modules", 1):
                // FSKit's own enablement state; a PlugInKit election is not it.
                DispatchQueue.global().asyncAfter(deadline: .now() + 10) {
                    fail(NSError(domain: NSPOSIXErrorDomain, code: Int(ETIMEDOUT)))
                }
                FSClient.shared.fetchInstalledExtensions { modules, error in
                    if let error { fail(error) }
                    guard let modules else {
                        fail(NSError(domain: NSPOSIXErrorDomain, code: Int(EIO)))
                    }
                    do {
                        try writeJSON(modules.filter {
                            $0.bundleIdentifier == "org.machlin.btrfs.filesystem"
                        }.map {
                            ["bundleIdentifier": $0.bundleIdentifier,
                             "enabled": $0.isEnabled, "path": $0.url.path] as [String: Any]
                        })
                    } catch { fail(error) }
                    exit(EXIT_SUCCESS)
                }
                dispatchMain()
            default:
                FileHandle.standardError.write(Data((usage + "\n").utf8))
                exit(EX_USAGE)
            }
            try writeJSON(result)
            exit(EXIT_SUCCESS)
        } catch {
            fail(error)
        }
    }
}
