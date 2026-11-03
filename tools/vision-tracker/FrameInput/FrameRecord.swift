// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
import Foundation

public enum FrameEncoding: UInt32 {
    case rgb565 = 1
    case jpeg = 2
}

public struct FrameRecord {
    public let encoding: FrameEncoding
    public let data: Data
    public init(encoding: FrameEncoding, data: Data) {
        self.encoding = encoding
        self.data = data
    }

    public static func read(using readChunk: (Int) throws -> Data?) throws -> FrameRecord? {
        func exact(_ size: Int, allowEOF: Bool = false) throws -> Data? {
            var data = Data()
            while data.count < size {
                guard let chunk = try readChunk(size - data.count), !chunk.isEmpty else {
                    if data.isEmpty && allowEOF { return nil }
                    throw FrameInputError.truncatedFrame
                }
                guard chunk.count <= size - data.count else { throw FrameInputError.invalidFrameSize }
                data.append(chunk)
            }
            return data
        }
        guard let header = try exact(8, allowEOF: true) else { return nil }
        func word(_ offset: Int) -> UInt32 {
            (0..<4).reduce(0) { $0 | UInt32(header[offset + $1]) << ($1 * 8) }
        }
        guard let encoding = FrameEncoding(rawValue: word(0)) else { throw FrameInputError.invalidFrameSize }
        let size = Int(word(4))
        guard encoding == .rgb565 ? size == RGB565Frame.byteCount : size > 0 && size <= 65536 else {
            throw FrameInputError.invalidFrameSize
        }
        return FrameRecord(encoding: encoding, data: try exact(size)!)
    }
}

public struct FaceBox: Equatable {
    public let x, y, width, height, confidence: Double
    public init(x: Double, y: Double, width: Double, height: Double, confidence: Double) {
        self.x = x; self.y = y; self.width = width; self.height = height; self.confidence = confidence
    }
}

public struct FaceTargetSelector {
    private var previous: FaceBox?
    private var lastSeen: Double = 0
    private var misses = 0
    public init() {}

    public mutating func select(_ faces: [FaceBox], timestamp: Double) -> FaceBox? {
        let valid = faces.filter { face in
            [face.x, face.y, face.width, face.height, face.confidence].allSatisfy { $0.isFinite && $0 >= 0 && $0 <= 1 }
                && face.confidence >= 0.55 && face.width > 0.025 && face.height > 0.025
        }
        if timestamp - lastSeen > 1.5 { previous = nil }
        var chosen: FaceBox?
        if let old = previous {
            let matches = valid.filter {
                let ratio = $0.width * $0.height / (old.width * old.height)
                return hypot($0.x - old.x, $0.y - old.y) < max(0.3, old.width)
                    && ratio > 0.35 && ratio < 2.8
            }
            chosen = matches.max { score($0, old) < score($1, old) }
            if chosen == nil {
                misses += 1
                if misses < 3 { return nil }
            }
        }
        if chosen == nil {
            chosen = valid.max { $0.width * $0.height * $0.confidence < $1.width * $1.height * $1.confidence }
        }
        if let chosen {
            previous = chosen; lastSeen = timestamp; misses = 0
        } else {
            previous = nil
        }
        return chosen
    }

    private func score(_ face: FaceBox, _ old: FaceBox) -> Double {
        let left = max(face.x - face.width / 2, old.x - old.width / 2)
        let right = min(face.x + face.width / 2, old.x + old.width / 2)
        let bottom = max(face.y - face.height / 2, old.y - old.height / 2)
        let top = min(face.y + face.height / 2, old.y + old.height / 2)
        let intersection = max(0, right - left) * max(0, top - bottom)
        let union = face.width * face.height + old.width * old.height - intersection
        return 3 * intersection / max(union, 0.0001) - hypot(face.x - old.x, face.y - old.y) + face.confidence * 0.1
    }
}
