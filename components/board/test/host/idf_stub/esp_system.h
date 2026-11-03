// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

using shutdown_handler_t = void (*)();
int esp_register_shutdown_handler(shutdown_handler_t handler);
