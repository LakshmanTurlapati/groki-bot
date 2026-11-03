// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
import Foundation

public enum FrameInputError: Error {
    case truncatedFrame
    case invalidFrameSize
}

public enum RGB565Frame {
    public static let width = 320
    public static let height = 240
    public static let byteCount = width * height * 2

    /// A clean EOF ends the worker; an incomplete record cannot be resynchronized.
    public static func read(using readChunk: (Int) throws -> Data?) throws -> Data? {
        var frame = Data()
        frame.reserveCapacity(byteCount)
        while frame.count < byteCount {
            guard let chunk = try readChunk(byteCount - frame.count), !chunk.isEmpty else {
                if frame.isEmpty { return nil }
                throw FrameInputError.truncatedFrame
            }
            guard chunk.count <= byteCount - frame.count else { throw FrameInputError.invalidFrameSize }
            frame.append(chunk)
        }
        return frame
    }

    public static func bgra(_ frame: Data) throws -> Data {
        guard frame.count == byteCount else { throw FrameInputError.invalidFrameSize }
        var pixels = Data(count: width * height * 4)
        frame.withUnsafeBytes { input in
            pixels.withUnsafeMutableBytes { output in
                let source = input.bindMemory(to: UInt8.self)
                let target = output.bindMemory(to: UInt8.self)
                for pixel in 0..<(width * height) {
                    let value = UInt16(source[pixel * 2]) << 8 | UInt16(source[pixel * 2 + 1])
                    target[pixel * 4] = UInt8((value & 0x1f) * 255 / 31)
                    target[pixel * 4 + 1] = UInt8(((value >> 5) & 0x3f) * 255 / 63)
                    target[pixel * 4 + 2] = UInt8(((value >> 11) & 0x1f) * 255 / 31)
                    target[pixel * 4 + 3] = 255
                }
            }
        }
        return pixels
    }
}
