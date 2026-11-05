// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0

#include "usb_voice_service.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>
#include <esp_jpeg_enc.h>

#include "camera_service.hpp"

#include "conversation/usb_voice_framer.hpp"
#include "conversation/usb_voice_port.hpp"
#include "conversation/usb_voice_lease.hpp"

namespace stackchan::app {
namespace {

constexpr const char* kTag = "usb-voice";
constexpr char kMcpPrefix[] = "$MCP:";
std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

class UsbVoiceService final : public conversation::UsbVoicePort {
public:
    bool send_json(std::string_view json) override
    {
        if (!active()) {
            return false;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(json.data());
        const bool sent = write_frame(conversation::UsbVoiceFrameType::VoiceJson, {bytes, json.size()});
        if (!sent)
            disable_voice();
        return sent;
    }

    bool send_audio(std::span<const std::uint8_t> opus) override
    {
        if (!active()) {
            return false;
        }
        // Device to host. The conversation task only calls this while Listening,
        // so speaker playback and mic uplink do not run together (half duplex,
        // same as the Wi-Fi client).
        return write_frame(conversation::UsbVoiceFrameType::AudioUplink, opus);
    }

    // The host's renewable lease defines this session. The low-level SOF
    // presence sample can briefly go false while a connected host is busy;
    // using it here discarded snapshots partway through a healthy transfer.
    bool active() const override {
        return lease_.active(now_ms());
    }

    void run()
    {
        camera_queue_ = xQueueCreate(1, sizeof(CameraRequest));
        if (camera_queue_ && xTaskCreatePinnedToCoreWithCaps(&camera_task, "usb_camera", 12288, this, 2, nullptr, 0,
                                                             MALLOC_CAP_SPIRAM) != pdPASS) {
            vQueueDelete(camera_queue_);
            camera_queue_ = nullptr;
            ESP_LOGW(kTag, "USB camera worker could not be created");
        }
        conversation::usb_voice_set_port(this);
        conversation::UsbVoiceStreamParser parser;
        std::uint8_t chunk[512];
        while (true) {
            if (reset_parser_.exchange(false))
                parser.reset();
            if (!ensure_driver()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (conversation::usb_voice_requested() && !active()) {
                disable_voice();
                parser.reset();
                ESP_LOGI(kTag, "USB voice host disconnected or lease expired; returning to Wi-Fi");
            }
            const int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(200));
            if (n < 0) {
                disable_voice();
                parser.reset();
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
                } else if (active()) {
                    lease_.refresh(now_ms());
                    handle_frame(event.frame);
                }
            }
        }
    }

private:
    struct CameraRequest {
        std::uint32_t id;
        std::uint32_t generation;
        bool jpeg;
        bool binary = false, motion = false;
        std::uint8_t quality = 80;
    };

    static void camera_task(void* arg) {
        static_cast<UsbVoiceService*>(arg)->run_camera();
    }

    bool send_rpc(std::uint32_t id, cJSON* result) {
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(response, "id", id);
        cJSON_AddItemToObject(response, "result", result);
        return write_json_line(response);
    }

    bool write_json_line(cJSON* message) {
        char* printed = cJSON_PrintUnformatted(message);
        cJSON_Delete(message);
        if (!printed)
            return false;
        const std::string line = std::string("\n") + kMcpPrefix + " " + printed + "\n";
        cJSON_free(printed);
        return write_all(reinterpret_cast<const std::uint8_t*>(line.data()), line.size());
    }

    bool compress_snapshot(std::vector<std::uint8_t>& pixels, std::uint8_t quality) {
        // The S3 SIMD encoder requires a 16-byte-aligned input image.
        auto* aligned =
            static_cast<std::uint8_t*>(heap_caps_aligned_alloc(16, pixels.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!aligned)
            return false;
        std::memcpy(aligned, pixels.data(), pixels.size());
        jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
        config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        config.subsampling = JPEG_SUBSAMPLE_420;
        config.quality = quality;
        jpeg_enc_handle_t encoder = nullptr;
        std::vector<std::uint8_t> encoded(65536);
        int length = 0;
        bool ok = jpeg_enc_open(&config, &encoder) == JPEG_ERR_OK;
        if (ok) {
            ok = jpeg_enc_process(encoder, aligned, static_cast<int>(pixels.size()), encoded.data(),
                                  static_cast<int>(encoded.size()), &length) == JPEG_ERR_OK;
            jpeg_enc_close(encoder);
        }
        heap_caps_free(aligned);
        if (!ok || length <= 0 || length > static_cast<int>(encoded.size()))
            return false;
        encoded.resize(length);
        pixels.swap(encoded);
        return true;
    }

    void run_camera() {
        CameraRequest request{};
        for (;;) {
            if (xQueueReceive(camera_queue_, &request, portMAX_DELAY) != pdTRUE)
                continue;
            auto current = [&] { return active() && conversation::usb_voice_generation() == request.generation; };
            if (!current()) {
                ESP_LOGW(kTag, "camera request %lu discarded before capture (session changed)",
                         static_cast<unsigned long>(request.id));
                continue;
            }
            std::vector<std::uint8_t> pixels;
            std::size_t width = 0, height = 0;
            std::string format;
            wifi_config::CameraCaptureMetadata metadata;
            std::uint32_t encode_ms = 0;
            std::function<bool(std::vector<std::uint8_t>&)> transform;
            if (request.jpeg)
                transform = [&](auto& frame) {
                    const auto started = now_ms();
                    const bool ok = compress_snapshot(frame, request.quality);
                    encode_ms = now_ms() - started;
                    return ok;
                };
            const bool captured = camera_service::capture_tracking_frame(pixels, width, height, format, metadata,
                                                                         transform, request.motion);
            if (!current()) {
                ESP_LOGW(kTag, "camera request %lu discarded after capture (session changed)",
                         static_cast<unsigned long>(request.id));
                continue;
            }
            cJSON* result = cJSON_CreateObject();
            if (!captured || pixels.empty() || (!request.jpeg && pixels.size() != 320 * 240 * 2)) {
                cJSON_AddBoolToObject(result, "busy", metadata.tracking_busy);
                cJSON_AddBoolToObject(result, "sleeping", metadata.tracking_sleeping);
                cJSON_AddStringToObject(result, "error", metadata.tracking_sleeping ? "sleeping"
                                                         : metadata.tracking_busy   ? "busy"
                                                                                    : "capture_failed");
                if (!send_rpc(request.id, result))
                    ESP_LOGW(kTag, "camera request %lu status write failed", static_cast<unsigned long>(request.id));
                continue;
            }
            if (request.jpeg)
                format = "jpeg";
            cJSON_AddNumberToObject(result, "encode_ms", encode_ms);
            cJSON_AddBoolToObject(result, "binary", request.binary);
            cJSON_AddNumberToObject(result, "jpeg_quality", request.jpeg ? request.quality : 0);
            bool sent = true;
            std::size_t transferred = 0;
            unsigned char encoded[2733];
            for (std::size_t offset = 0; offset < pixels.size(); offset += 2048) {
                if (!current()) {
                    sent = false;
                    break;
                }
                const auto count = std::min<std::size_t>(2048, pixels.size() - offset);
                if (request.binary) {
                    std::vector<std::uint8_t> payload(8 + count);
                    for (int byte = 0; byte < 4; ++byte) {
                        payload[byte] = static_cast<std::uint8_t>(request.id >> (8 * byte));
                        payload[4 + byte] = static_cast<std::uint8_t>(offset >> (8 * byte));
                    }
                    std::memcpy(payload.data() + 8, pixels.data() + offset, count);
                    if (!write_frame(conversation::UsbVoiceFrameType::CameraChunk, payload)) {
                        sent = false;
                        break;
                    }
                    transferred += count;
                    continue;
                }
                std::size_t length = 0;
                if (mbedtls_base64_encode(encoded, sizeof(encoded), &length, pixels.data() + offset,
                                          std::min<std::size_t>(2048, pixels.size() - offset)) != 0) {
                    sent = false;
                    break;
                }
                encoded[length] = 0;
                cJSON* message = cJSON_CreateObject();
                cJSON_AddStringToObject(message, "jsonrpc", "2.0");
                cJSON_AddStringToObject(message, "method", "camera/chunk");
                cJSON* params = cJSON_AddObjectToObject(message, "params");
                cJSON_AddNumberToObject(params, "id", request.id);
                cJSON_AddNumberToObject(params, "offset", offset);
                cJSON_AddStringToObject(params, "data", reinterpret_cast<const char*>(encoded));
                if (!write_json_line(message)) {
                    sent = false;
                    break;
                }
                transferred += std::min<std::size_t>(2048, pixels.size() - offset);
            }
            if (!sent) {
                ESP_LOGW(kTag, "camera request %lu transfer stopped at %u/%u bytes",
                         static_cast<unsigned long>(request.id), static_cast<unsigned>(transferred),
                         static_cast<unsigned>(pixels.size()));
                cJSON_Delete(result);
                continue;
            }
            cJSON_AddStringToObject(result, "frame_id", metadata.frame_id.c_str());
            cJSON_AddNumberToObject(result, "width", width);
            cJSON_AddNumberToObject(result, "height", height);
            cJSON_AddStringToObject(result, "format", format.c_str());
            cJSON_AddNumberToObject(result, "bytes", pixels.size());
            cJSON_AddNumberToObject(result, "capture_ms", metadata.capture_ms);
            cJSON_AddNumberToObject(result, "capture_yaw", metadata.capture_yaw);
            cJSON_AddNumberToObject(result, "capture_pitch", metadata.capture_pitch);
            cJSON_AddBoolToObject(result, "motion_compensated", metadata.motion_compensated);
            cJSON_AddNumberToObject(result, "capture_motion_deg", metadata.capture_motion_deg);
            if (!send_rpc(request.id, result))
                ESP_LOGW(kTag, "camera request %lu result write failed", static_cast<unsigned long>(request.id));
        }
    }

    bool ensure_driver()
    {
        if (driver_ready_) {
            return true;
        }
        // An installed driver may still be in use by the console. Preserve
        // its ownership; normal boot has no driver, so we install the larger
        // rings below before directing VFS through them.
        if (usb_serial_jtag_is_driver_installed()) {
            usb_serial_jtag_vfs_use_driver();
            driver_ready_ = true;
            return true;
        }
        usb_serial_jtag_driver_config_t cfg{};
        // JSON frames can be 4096 + 8 bytes. IDF queues each write atomically
        // (all bytes or zero); the TX ring must fit the largest whole frame.
        cfg.tx_buffer_size = 8192;
        cfg.rx_buffer_size = 4096;
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            // Keep the console alive if the larger rings do not fit.
            usb_serial_jtag_driver_config_t fallback{};
            fallback.tx_buffer_size = 8192;
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

    void disable_voice() {
        lease_.disable();
        conversation::usb_voice_set_requested(false);
        reset_parser_.store(true);
    }

    bool write_all(const std::uint8_t* data, std::size_t len)
    {
        std::lock_guard lock{tx_mu_};
        // Camera chunks, diagnostics, and voice share this ring. Allow a
        // bounded host scheduling stall before abandoning a voice session.
        const int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(500));
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
        if (cJSON_IsString(method) && std::strcmp(method->valuestring, "camera/capture") == 0) {
            const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
            if (cJSON_IsNumber(id) && std::isfinite(id->valuedouble) && id->valuedouble >= 1 &&
                id->valuedouble <= 2147483647 && std::floor(id->valuedouble) == id->valuedouble) {
                const auto* params = cJSON_GetObjectItemCaseSensitive(root, "params");
                const auto* encoding = cJSON_GetObjectItemCaseSensitive(params, "format");
                CameraRequest request{static_cast<std::uint32_t>(id->valuedouble), conversation::usb_voice_generation(),
                                      cJSON_IsString(encoding) && std::strcmp(encoding->valuestring, "jpeg") == 0};
                request.binary = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "binary"));
                request.motion = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "motion"));
                const auto* quality = cJSON_GetObjectItemCaseSensitive(params, "quality");
                if (cJSON_IsNumber(quality) && std::isfinite(quality->valuedouble) && quality->valuedouble >= 60 &&
                    quality->valuedouble <= 90 && std::floor(quality->valuedouble) == quality->valuedouble)
                    request.quality = static_cast<std::uint8_t>(quality->valuedouble);
                if (!active() || !camera_queue_ || xQueueSend(camera_queue_, &request, 0) != pdTRUE) {
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddBoolToObject(result, "busy", true);
                    send_rpc(request.id, result);
                }
            }
        } else if (cJSON_IsString(method) && std::strcmp(method->valuestring, "voice/ping") == 0) {
            const bool renewed = active() && lease_.refresh(now_ms());
            if (!renewed)
                disable_voice();
            reply_mode(root, renewed, renewed);
        } else if (cJSON_IsString(method) && std::strcmp(method->valuestring, "voice/mode") == 0) {
            bool enable = true;
            const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
            if (cJSON_IsObject(params)) {
                const cJSON* flag = cJSON_GetObjectItemCaseSensitive(params, "enable");
                if (cJSON_IsBool(flag)) {
                    enable = cJSON_IsTrue(flag);
                }
            }
            if (enable) {
                const bool starting = lease_.enable(now_ms());
                reply_mode(root, true, true);
                // Hello follows the ack. If the XiaoZhi client is not in USB
                // mode yet, the flag stays latched and bind sends hello once.
                if (starting) {
                    ESP_LOGI(kTag, "host enabled USB voice");
                    conversation::usb_voice_notify_host_ready();
                }
            } else {
                disable_voice();
                ESP_LOGI(kTag, "host disabled USB voice; Wi-Fi WebSocket is the voice path");
                reply_mode(root, false, true);
            }
        }
        // Any other `$MCP:` method (tools/call, initialize, ...) is left
        // unanswered so the gateway's existing timeout and WebSocket fallback
        // stay as they are. This firmware has no USB tool server.
        cJSON_Delete(root);
    }

    void reply_mode(const cJSON* request, bool enable, bool ok) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "id");
        if (!cJSON_IsNumber(id)) {
            return;
        }
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        cJSON* result = cJSON_AddObjectToObject(resp, "result");
        cJSON_AddBoolToObject(result, "ok", ok);
        cJSON_AddStringToObject(result, "transport", enable ? "usb" : "websocket");
        cJSON_AddNumberToObject(result, "lease_ms", conversation::UsbVoiceLease::kDurationMs);
        // The serial pipe is full duplex. The conversation state machine is
        // not: mic uplink is dropped while the speaker is playing, same as Wi-Fi.
        cJSON_AddStringToObject(result, "duplex", "half");
        char* printed = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (printed == nullptr) {
            return;
        }
        // ESP_LOG writes characters through VFS; end any partial log prefix
        // before the atomic reply so the host always sees $MCP at line start.
        std::string line = std::string("\n") + kMcpPrefix + " " + printed + "\n";
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
    QueueHandle_t camera_queue_{nullptr};
    conversation::UsbVoiceLease lease_;
    std::atomic<bool> reset_parser_{false};
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
