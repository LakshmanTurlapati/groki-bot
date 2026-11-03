// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace stackchan::app {

// Reads the USB Serial/JTAG console for `$MCP:` lines and XiaoZhi voice
// frames. Started once at boot when conversation support is compiled in.
// Logs keep using the console; this task only consumes host-to-device bytes.
void start_usb_voice_service();

} // namespace stackchan::app
