// SPDX-License-Identifier: BSD-3-Clause
import ExtensionFoundation
import FSKit

@main
struct BtrfsExtension: UnaryFileSystemExtension {
    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
        BtrfsFileSystem()
    }
}
