// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace stackchan::board {

enum class BoardKind { M5Base, AtomS3 };

struct BoardProfile {
    bool has_camera;
};

inline BoardProfile profile_for(BoardKind kind) { return {kind == BoardKind::M5Base}; }

class Board {
public:
    explicit Board(BoardKind kind = BoardKind::M5Base) : kind_(kind) {}
    BoardKind kind() const { return kind_; }

private:
    BoardKind kind_;
};

} // namespace stackchan::board
