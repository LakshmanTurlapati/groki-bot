// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
import Foundation
import Testing
@testable import FrameInput

@Test func rgb565BigEndianPrimaryColorsBecomeBgra() throws {
    var frame = Data(repeating: 0, count: RGB565Frame.byteCount)
    frame.replaceSubrange(0..<10, with: [0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f, 0xff, 0xff, 0x00, 0x00])
    let pixels = try RGB565Frame.bgra(frame)
    #expect(Array(pixels.prefix(20)) == [
        0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255,
        255, 255, 255, 255, 0, 0, 0, 255,
    ])
}

@Test func partialReadsProduceExactlyOneFrameWithoutEatingTheNext() throws {
    let source = Data(repeating: 0x5a, count: RGB565Frame.byteCount * 2)
    var offset = 0
    func chunk(_ requested: Int) -> Data? {
        let count = min(requested, 113, source.count - offset)
        defer { offset += count }
        return source.subdata(in: offset..<(offset + count))
    }
    #expect(try RGB565Frame.read(using: chunk)?.count == RGB565Frame.byteCount)
    #expect(offset == RGB565Frame.byteCount)
    #expect(try RGB565Frame.read(using: chunk)?.count == RGB565Frame.byteCount)
    #expect(try RGB565Frame.read(using: chunk) == nil)
}

@Test func truncatedRecordsAndWrongPixelSizesAreRejected() {
    var first = true
    #expect(throws: FrameInputError.self) {
        try RGB565Frame.read { _ in
            defer { first = false }
            return first ? Data([0, 1]) : nil
        }
    }
    #expect(throws: FrameInputError.self) { try RGB565Frame.bgra(Data([0, 1])) }
}

@Test func mixedFrameRecordsAreBoundedAndStaySynchronized() throws {
    var data = Data([2, 0, 0, 0, 4, 0, 0, 0, 0xff, 0xd8, 0xff, 0xd9])
    data.append(contentsOf: [1, 0, 0, 0, 0, 88, 2, 0]) // 153600-byte RGB565 record
    data.append(Data(repeating: 0, count: RGB565Frame.byteCount))
    var position = 0
    func chunk(_ wanted: Int) -> Data? {
        let count = min(wanted, 57, data.count - position)
        defer { position += count }
        return data.subdata(in: position..<(position + count))
    }
    #expect(try FrameRecord.read(using: chunk)?.encoding == .jpeg)
    #expect(try FrameRecord.read(using: chunk)?.data.count == RGB565Frame.byteCount)
    #expect(try FrameRecord.read(using: chunk) == nil)
    #expect(throws: FrameInputError.self) {
        try FrameRecord.read { _ in Data([2, 0, 0, 0, 255, 255, 255, 127]) }
    }
}

@Test func faceSelectionRetainsTheTargetWhenAnotherFaceHasHigherConfidence() {
    var selector = FaceTargetSelector()
    let target = FaceBox(x: 0.3, y: 0.5, width: 0.3, height: 0.4, confidence: 0.8)
    let other = FaceBox(x: 0.8, y: 0.5, width: 0.15, height: 0.2, confidence: 0.95)
    #expect(selector.select([target, other], timestamp: 10) == target)
    let next = FaceBox(x: 0.32, y: 0.5, width: 0.3, height: 0.4, confidence: 0.65)
    #expect(selector.select([other, next], timestamp: 10.2) == next)
    #expect(selector.select([other], timestamp: 10.4) == nil)
    #expect(selector.select([other], timestamp: 10.6) == nil)
    #expect(selector.select([other], timestamp: 10.8) == other)
}

@Test func frameRecordsRejectTruncatedHeadersAndPayloads() {
    for source in [Data([2, 0, 0]), Data([2, 0, 0, 0, 4, 0, 0, 0, 0xff])] {
        var offset = 0
        #expect(throws: FrameInputError.self) {
            try FrameRecord.read { requested in
                let count = min(requested, source.count - offset)
                defer { offset += count }
                return source.subdata(in: offset..<(offset + count))
            }
        }
    }
}

@Test func pixelConversionPreservesTopAndBottomRows() throws {
    var frame = Data(repeating: 0, count: RGB565Frame.byteCount)
    frame.replaceSubrange(0..<2, with: [0xf8, 0x00])
    frame.replaceSubrange((RGB565Frame.byteCount - 2)..<RGB565Frame.byteCount, with: [0x00, 0x1f])
    let pixels = try RGB565Frame.bgra(frame)
    #expect(Array(pixels.prefix(4)) == [0, 0, 255, 255])
    #expect(Array(pixels.suffix(4)) == [255, 0, 0, 255])
}
