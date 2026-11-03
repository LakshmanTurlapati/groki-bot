// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "usb_voice_service.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "conversation/usb_voice_framer.hpp"
#include "conversation/usb_voice_port.hpp"

namespace stackchan::app {
namespace {

constexpr const char* kTag = "usb-voice";
constexpr char kMcpPrefix[] = "$MCP:";

class UsbVoiceService final : public conversation::UsbVoicePort {
public:
    bool send_json(std::string_view json) override
    {
        if (!voice_on_.load(std::memory_order_acquire)) {
            return false;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(json.data());
        return write_frame(conversation::UsbVoiceFrameType::VoiceJson, {bytes, json.size()});
    }

    bool send_audio(std::span<const std::uint8_t> opus) override
    {
        if (!voice_on_.load(std::memory_order_acquire)) {
            return false;
        }
        // Device to host. The conversation task only calls this while Listening,
        // so speaker playback and mic uplink do not run together (half duplex,
        // same as the Wi-Fi client).
        return write_frame(conversation::UsbVoiceFrameType::AudioUplink, opus);
    }

    bool active() const override { return voice_on_.load(std::memory_order_acquire); }

    void run()
    {
        conversation::usb_voice_set_port(this);
        conversation::UsbVoiceStreamParser parser;
        std::uint8_t chunk[512];
        while (true) {
            if (!ensure_driver()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            const int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(200));
            if (n < 0) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (n == 0) {
                continue;
            }
            parser.feed(std::span<const std::uint8_t>{chunk, static_cast<std::size_t>(n)});
            conversation::UsbVoiceEvent event;
            while (parser.pop(event)) {
                if (event.kind == conversation::UsbVoiceEventKind::Line) {
                    handle_line(event.line);
                } else if (voice_on_.load(std::memory_order_acquire)) {
                    handle_frame(event.frame);
                }
            }
        }
    }

private:
    bool ensure_driver()
    {
        if (driver_ready_) {
            return true;
        }
        // The console installs this driver at boot with a small ring. One
        // Opus frame is at most 1275 B plus an 8 B header, so reinstall with
        // a 4 KiB ring. write_bytes then queues a whole frame under the driver
        // lock and a log line cannot split it. VFS is pointed back at the
        // driver so ESP_LOG lines keep working.
        if (usb_serial_jtag_is_driver_installed()) {
            usb_serial_jtag_driver_uninstall();
        }
        usb_serial_jtag_driver_config_t cfg{};
        cfg.tx_buffer_size = 4096;
        cfg.rx_buffer_size = 4096;
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            // Keep the console alive if the larger rings do not fit.
            usb_serial_jtag_driver_config_t fallback{};
            fallback.tx_buffer_size = 2048;
            fallback.rx_buffer_size = 2048;
            err = usb_serial_jtag_driver_install(&fallback);
        }
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "driver install failed: %s", esp_err_to_name(err));
            return false;
        }
        usb_serial_jtag_vfs_use_driver();
        driver_ready_ = true;
        ESP_LOGI(kTag, "USB serial ready for voice frames");
        return true;
    }

    bool write_all(const std::uint8_t* data, std::size_t len)
    {
        std::lock_guard lock{tx_mu_};
        const int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
        return n == static_cast<int>(len);
    }

    bool write_frame(conversation::UsbVoiceFrameType type, std::span<const std::uint8_t> payload)
    {
        const auto frame = conversation::encode_usb_voice_frame(type, payload);
        if (frame.empty()) {
            return false;
        }
        return write_all(frame.data(), frame.size());
    }

    void handle_line(const std::string& line)
    {
        if (line.rfind(kMcpPrefix, 0) != 0) {
            return;
        }
        const char* json = line.c_str() + (sizeof(kMcpPrefix) - 1);
        while (*json == ' ') {
            ++json;
        }
        cJSON* root = cJSON_Parse(json);
        if (root == nullptr) {
            return;
        }
        const cJSON* method = cJSON_GetObjectItemCaseSensitive(root, "method");
        if (cJSON_IsString(method) && std::strcmp(method->valuestring, "voice/mode") == 0) {
            bool enable = true;
            const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
            if (cJSON_IsObject(params)) {
                const cJSON* flag = cJSON_GetObjectItemCaseSensitive(params, "enable");
                if (cJSON_IsBool(flag)) {
                    enable = cJSON_IsTrue(flag);
                }
            }
            if (enable) {
                voice_on_.store(true, std::memory_order_release);
                ESP_LOGI(kTag, "host enabled USB voice");
                reply_mode(root, true);
                // Hello follows the ack. If the XiaoZhi client is not in USB
                // mode yet, the flag stays latched and bind sends hello once.
                conversation::usb_voice_notify_host_ready();
            } else {
                voice_on_.store(false, std::memory_order_release);
                conversation::usb_voice_set_requested(false);
                ESP_LOGI(kTag, "host disabled USB voice; Wi-Fi WebSocket is the voice path");
                reply_mode(root, false);
            }
        }
        // Any other `$MCP:` method (tools/call, initialize, ...) is left
        // unanswered so the gateway's existing timeout and WebSocket fallback
        // stay as they are. This firmware has no USB tool server.
        cJSON_Delete(root);
    }

    void reply_mode(const cJSON* request, bool enable)
    {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "id");
        if (!cJSON_IsNumber(id)) {
            return;
        }
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        cJSON* result = cJSON_AddObjectToObject(resp, "result");
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddStringToObject(result, "transport", enable ? "usb" : "websocket");
        // The serial pipe is full duplex. The conversation state machine is
        // not: mic uplink is dropped while the speaker is playing, same as Wi-Fi.
        cJSON_AddStringToObject(result, "duplex", "half");
        char* printed = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (printed == nullptr) {
            return;
        }
        std::string line = std::string(kMcpPrefix) + " " + printed + "\n";
        cJSON_free(printed);
        (void)write_all(reinterpret_cast<const std::uint8_t*>(line.data()), line.size());
    }

    void handle_frame(const conversation::UsbVoiceFrame& frame)
    {
        if (frame.type == conversation::UsbVoiceFrameType::AudioDownlink) {
            conversation::usb_voice_dispatch_audio(frame.payload);
        } else if (frame.type == conversation::UsbVoiceFrameType::VoiceJson && !frame.payload.empty()) {
            const auto* text = reinterpret_cast<const char*>(frame.payload.data());
            conversation::usb_voice_dispatch_json(std::string_view{text, frame.payload.size()});
        }
    }

    std::mutex tx_mu_;
    std::atomic<bool> voice_on_{false};
    bool driver_ready_{false};
};

void service_task(void* /*arg*/)
{
    static UsbVoiceService service;
    service.run();
    vTaskDelete(nullptr);
}

} // namespace

void start_usb_voice_service()
{
    if (xTaskCreatePinnedToCoreWithCaps(&service_task, "usb_voice", 16384, nullptr, 4, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
    }
}

} // namespace stackchan::app
