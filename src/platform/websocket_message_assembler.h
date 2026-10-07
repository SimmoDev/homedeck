#pragma once

#include "platform/websocket_client.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace homedeck {

// Bounds how many complete messages can accumulate before the oldest is
// dropped. A peer that sends unsolicited messages (Kodi's notifications, a
// hub's stray frames) faster than the consumer drains them would otherwise
// grow the queue without bound on a device that stays up for weeks. The
// consumers' bounded drain loops (HarmonyConnection::DrainStaleMessages(),
// KodiClient::PumpNotifications()) each run at least this many iterations,
// so a full queue empties in one drain - keep them at or above this value.
constexpr size_t kMaxQueuedWebSocketMessages = 20;

// Turns the chunks a callback-driven WebSocket library delivers into
// complete messages: a message may span several frames (a continuation
// frame follows one whose FIN bit is unset), and one frame may arrive in
// several chunks. Enforces kMaxWebSocketMessageBytes and
// kMaxQueuedWebSocketMessages. Not thread-safe - the owner serialises
// access. Portable so the logic is host-tested; FirmwareWebSocketClient
// supplies the chunks.
class WebSocketMessageAssembler {
public:
    enum class Result {
        kPartial,    // more chunks are needed
        kComplete,   // a message was queued
        kOversized,  // the message exceeded the bound and was discarded
    };

    // `fin`: the chunk belongs to the final frame of its message.
    // `frame_complete`: the chunk ends its frame (offset + length reached
    // the frame's payload length).
    Result Add(const char* data, size_t length, bool fin, bool frame_complete) {
        // Checked before appending so the buffer never exceeds the bound.
        if (in_progress_.size() + length > kMaxWebSocketMessageBytes) {
            in_progress_.clear();
            return Result::kOversized;
        }
        in_progress_.append(data, length);
        if (!fin || !frame_complete) {
            return Result::kPartial;
        }
        queue_.push_back(std::move(in_progress_));
        in_progress_.clear();
        while (queue_.size() > kMaxQueuedWebSocketMessages) {
            queue_.pop_front();
        }
        return Result::kComplete;
    }

    std::optional<std::string> Pop() {
        if (queue_.empty()) {
            return std::nullopt;
        }
        std::string message = std::move(queue_.front());
        queue_.pop_front();
        return message;
    }

    bool HasMessage() const { return !queue_.empty(); }

    void Clear() {
        queue_.clear();
        in_progress_.clear();
    }

private:
    std::deque<std::string> queue_;
    std::string in_progress_;
};

}  // namespace homedeck
