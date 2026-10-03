#include "cyane/net/connection.hpp"

#include <algorithm>
#include <vector>

#include "cyane/core/log.hpp"
#include "connection_detail.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/proto/json.hpp"
#include "cyane/world/blocks.hpp"

namespace cyane::net {

Connection::Connection(Socket socket, std::string peer, Reactor& reactor, ConnectionContext context)
    : socket_(std::move(socket)), peer_(std::move(peer)), reactor_(&reactor),
      context_(std::move(context)), inbox_(kMaxInboxBytes), scratch_() {
    log::debug("connection {} -> {}", fd(), peer_);
}

Connection::~Connection() {
    teardown();
}

void Connection::on_readable() {
    while (true) {
        IoResult io = socket_.try_read(MutableByteSpan{inbox_}.subspan(inbox_offset_));
        if (io.status == IoStatus::closed) {
            teardown();
            return;
        }
        if (io.status == IoStatus::error) {
            log::warn("connection {} read error", fd());
            teardown();
            return;
        }
        if (io.status == IoStatus::would_block) {
            break;
        }
        inbox_offset_ += io.bytes;
        process_inbox();
    }
    // 处理完入站数据后立即尝试冲刷出站队列，避免依赖 EPOLLOUT 边沿时序
    if (alive_ && outbox_offset_ > 0) {
        want_write_ = true;
        flush_outbox();
    }
}

void Connection::on_writable() {
    flush_outbox();
}

void Connection::on_error() {
    log::warn("connection {} error", fd());
    teardown();
}

void Connection::teardown() noexcept {
    if (!alive_) {
        return;
    }
    alive_ = false;
    save_player_data();
    broadcast_despawn();
    if (context_.player_manager != nullptr && player_id_ != 0) {
        context_.player_manager->remove(player_id_);
    }
    if (reactor_ != nullptr) {
        reactor_->remove(fd());
    }
    socket_.close();
}

void Connection::process_inbox() {
    // inbox_offset_ 表示已缓冲的有效字节数；cursor 是本轮解析的读游标
    std::size_t cursor = 0;
    while (cursor < inbox_offset_) {
        ByteReader reader{ByteSpan{inbox_.data() + cursor, inbox_offset_ - cursor}};
        auto maybe_frame_size = reader.varint();
        if (!maybe_frame_size) {
            // 帧长度前缀不完整，等待更多数据
            break;
        }
        const std::int32_t frame_size = *maybe_frame_size;
        if (frame_size < 0 || frame_size > proto::kMaxFrameBytes) {
            log::warn("connection {} bad frame length {}", fd(), frame_size);
            teardown();
            return;
        }

        const std::size_t header_size = reader.offset();
        const std::size_t total_size = header_size + static_cast<std::size_t>(frame_size);
        if (cursor + total_size > inbox_offset_) {
            // 帧不完整，等待更多数据
            break;
        }

        ByteSpan frame_body{inbox_.data() + cursor + header_size, static_cast<std::size_t>(frame_size)};
        auto decoded = proto::decode_frame(frame_body, scratch_, compression_threshold_);
        if (!decoded) {
            log::warn("connection {} invalid frame: {}", fd(), decoded.error().message);
            teardown();
            return;
        }
        if (!handle_packet(decoded->packet_id, decoded->payload)) {
            teardown();
            return;
        }
        cursor += total_size;
    }
    // 将未消费的尾部数据挪到缓冲区开头
    if (cursor > 0) {
        const std::size_t leftover = inbox_offset_ - cursor;
        if (leftover > 0) {
            std::memmove(inbox_.data(), inbox_.data() + cursor, leftover);
        }
        inbox_offset_ = leftover;
    }
}

bool Connection::handle_packet(std::int32_t packet_id, ByteSpan payload) {
    if (state_ == proto::State::handshake) {
        return handle_handshake(payload);
    }
    if (state_ == proto::State::status) {
        return handle_status(packet_id, payload);
    }
    if (state_ == proto::State::login) {
        return handle_login(packet_id, payload);
    }
    if (state_ == proto::State::play) {
        return handle_play(packet_id, payload);
    }
    log::warn("connection {} unknown state", fd());
    return false;
}

void Connection::tick(std::uint64_t now_ms) {
    if (!alive_ || state_ != proto::State::play) {
        return;
    }
    now_ms_ = now_ms;
    // 先投递他人广播来的消息（进入 play 后 hub_entry_ 有效）
    drain_mailbox();
    // 检测并拾取附近掉落物
    collect_items(now_ms);
    // 熔炉窗口打开时同步燃烧/冶炼进度条
    sync_furnace_progress();
    // 限流发出待发表中的区块（避免跨区块/登录时一次性灌爆 outbox）
    send_pending_chunks(kChunkPerTick);
    // 按钮回弹：到期后清除 0x8 位并广播
    if (!pressed_buttons_.empty()) {
        for (std::size_t i = 0; i < pressed_buttons_.size();) {
            const auto [bkey, release_ms] = pressed_buttons_[i];
            if (now_ms < release_ms) {
                ++i;
                continue;
            }
            const auto [bx, by, bz] = world::unpack_block_pos(bkey);
            if (context_.world != nullptr) {
                const auto st = context_.world->block_at(bx, by, bz);
                const auto cid = world::block_id(st);
                if (cid == 77 || cid == 143) {
                    set_block_and_broadcast(bx, by, bz, static_cast<std::uint16_t>(st & ~0x08));
                }
            }
            pressed_buttons_[i] = pressed_buttons_.back();
            pressed_buttons_.pop_back();
        }
    }
    // 首次进入 play：以当前时间作为存活基线
    if (last_keepalive_recv_ms_ == 0) {
        last_keepalive_recv_ms_ = now_ms;
        last_keepalive_sent_ms_ = now_ms;
    }
    // 已收到回复：刷新存活基线
    if (!awaiting_keepalive_) {
        last_keepalive_recv_ms_ = now_ms;
    }
    // 超时未回：断开
    if (awaiting_keepalive_ && now_ms - last_keepalive_recv_ms_ > kKeepAliveTimeoutMs) {
        disconnect("Timed out");
        return;
    }
    // 到间隔且上一个已回：发新的 KeepAlive（用 now 的低位做 id）
    if (!awaiting_keepalive_ && now_ms - last_keepalive_sent_ms_ >= kKeepAliveIntervalMs) {
        last_keepalive_id_ = static_cast<std::int64_t>(now_ms);
        last_keepalive_sent_ms_ = now_ms;
        awaiting_keepalive_ = true;
        cyane::ByteWriter fields;
        fields.i64(last_keepalive_id_);
        send_packet(proto::play_cb::kKeepAlive, fields.data());
    }
}

void Connection::drain_mailbox() {
    if (!hub_entry_) {
        return;
    }
    std::vector<HubMessage> pending;
    {
        std::lock_guard<std::mutex> lock(hub_entry_->mailbox_mutex);
        pending.swap(hub_entry_->mailbox);
    }
    for (const auto& msg : pending) {
        if (msg.kill_flag) {
            kill_player();
            continue;
        }
        if (msg.gamemode >= 0) {
            apply_remote_gamemode(static_cast<std::uint8_t>(msg.gamemode));
            continue;
        }
        send_packet(msg.packet_id, ByteSpan{msg.payload});
    }
}

void Connection::enable_cipher(ByteSpan session_key) {
    auto decrypt_result = crypto::StreamCipher::aes_cfb8(session_key, false);
    if (!decrypt_result) {
        teardown();
        return;
    }
    decrypt_cipher_ = std::make_unique<crypto::StreamCipher>(std::move(*decrypt_result));

    auto encrypt_result = crypto::StreamCipher::aes_cfb8(session_key, true);
    if (!encrypt_result) {
        teardown();
        return;
    }
    encrypt_cipher_ = std::make_unique<crypto::StreamCipher>(std::move(*encrypt_result));
}

void Connection::flush_outbox() {
    if (!want_write_ || outbox_offset_ == 0) {
        return;
    }
    IoResult io = socket_.try_write(ByteSpan{outbox_.data(), outbox_offset_});
    if (io.status == IoStatus::error) {
        log::warn("connection {} write error", fd());
        teardown();
        return;
    }
    if (io.status == IoStatus::would_block) {
        return;
    }
    outbox_offset_ -= io.bytes;
    if (outbox_offset_ > 0) {
        std::memmove(outbox_.data(), outbox_.data() + io.bytes, outbox_offset_);
    }
    if (outbox_offset_ == 0) {
        want_write_ = false;
        (void)reactor_->set_writable(fd(), *this, false);
        if (close_after_flush_) {
            teardown();
        }
    }
}

void Connection::set_writable(bool writable) {
    if (writable != want_write_) {
        want_write_ = writable;
        (void)reactor_->set_writable(fd(), *this, writable);
    }
}

void Connection::send_packet(std::int32_t packet_id, ByteSpan fields) {
    if (!alive_) {
        return;
    }
    Bytes frame;
    frame.reserve(fields.size() + varint_size(static_cast<std::int32_t>(fields.size())) + varint_size(packet_id));
    proto::encode_frame(frame, packet_id, fields, compression_threshold_);
    if (frame.size() > kOutboxHighWater) {
        log::warn("connection {} outbox too large", fd());
        teardown();
        return;
    }
    if (outbox_offset_ + frame.size() > outbox_.size()) {
        outbox_.resize(std::max(outbox_.size() * 2, outbox_offset_ + frame.size()));
    }
    std::memcpy(outbox_.data() + outbox_offset_, frame.data(), frame.size());
    outbox_offset_ += frame.size();
    set_writable(true);
}

void Connection::disconnect(std::string_view reason) {
    if (!alive_) {
        return;
    }
    cyane::ByteWriter fields;
    fields.string(proto::chat_text(reason));
    if (state_ == proto::State::login) {
        send_packet(proto::login_cb::kDisconnect, fields.data());
    } else if (state_ == proto::State::play) {
        send_packet(proto::play_cb::kDisconnect, fields.data());
    }
    close_after_flush_ = true;
    flush_outbox();
    if (outbox_offset_ == 0) {
        teardown();
    }
}

}
