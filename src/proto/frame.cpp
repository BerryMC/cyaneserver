#include "cyane/proto/frame.hpp"

#if CYANE_HAVE_ZLIB
#include <zlib.h>
#endif

#include <format>

namespace cyane::proto {

Result<Bytes> deflate(ByteSpan input, int level) {
#if CYANE_HAVE_ZLIB
    uLongf bound = compressBound(static_cast<uLong>(input.size()));
    Bytes output(static_cast<std::size_t>(bound));
    const int status = compress2(reinterpret_cast<Bytef*>(output.data()),
                                 &bound,
                                 reinterpret_cast<const Bytef*>(input.data()),
                                 static_cast<uLong>(input.size()),
                                 level);
    if (status != Z_OK) {
        return make_error(ErrorCode::protocol, std::format("deflate failed with zlib status {}", status));
    }
    output.resize(static_cast<std::size_t>(bound));
    return output;
#else
    (void)input;
    (void)level;
    return make_error(ErrorCode::protocol, "built without zlib-ng, compression unavailable");
#endif
}

Result<Bytes> inflate(ByteSpan input, std::size_t output_size) {
#if CYANE_HAVE_ZLIB
    Bytes output(output_size);
    uLongf produced = static_cast<uLongf>(output_size);
    const int status = uncompress(reinterpret_cast<Bytef*>(output.data()),
                                  &produced,
                                  reinterpret_cast<const Bytef*>(input.data()),
                                  static_cast<uLong>(input.size()));
    if (status != Z_OK) {
        return make_error(ErrorCode::protocol, std::format("inflate failed with zlib status {}", status));
    }
    output.resize(static_cast<std::size_t>(produced));
    return output;
#else
    (void)input;
    (void)output_size;
    return make_error(ErrorCode::protocol, "built without zlib-ng, compression unavailable");
#endif
}

Result<Bytes> inflate_dynamic(ByteSpan input, std::size_t max_output, bool gzip) {
#if CYANE_HAVE_ZLIB
    z_stream stream{};
    // 窗口位 15；gzip 载荷（Anvil 版本字节 1）需 +16，zlib 载荷（版本字节 2）用 15
    if (inflateInit2(&stream, gzip ? 15 + 16 : 15) != Z_OK) {
        return make_error(ErrorCode::protocol, "inflate_dynamic: init failed");
    }
    const std::unique_ptr<z_stream, decltype(&inflateEnd)> guard{&stream, inflateEnd};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    Bytes output;
    std::array<std::byte, 65536> chunk{};
    while (true) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        const int status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = chunk.size() - stream.avail_out;
        if (produced > 0) {
            if (output.size() + produced > max_output) {
                return make_error(ErrorCode::protocol,
                                  std::format("inflate_dynamic output exceeds limit {}", max_output));
            }
            output.insert(output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        }
        if (status == Z_STREAM_END) {
            return output;
        }
        if (status != Z_OK) {
            return make_error(ErrorCode::protocol, std::format("inflate_dynamic failed with zlib status {}", status));
        }
        if (stream.avail_in == 0 && produced == 0) {
            return make_error(ErrorCode::protocol, "inflate_dynamic: truncated stream");
        }
    }
#else
    (void)input;
    (void)max_output;
    (void)gzip;
    return make_error(ErrorCode::protocol, "built without zlib-ng, compression unavailable");
#endif
}

Result<DecodedFrame> decode_frame(ByteSpan body, Bytes& scratch, std::int32_t threshold) {
    ByteReader reader{body};
    if (threshold < 0) {
        auto packet_id = reader.varint();
        if (!packet_id) {
            return std::unexpected{std::move(packet_id.error())};
        }
        return DecodedFrame{*packet_id, reader.rest()};
    }

    auto data_length = reader.varint();
    if (!data_length) {
        return std::unexpected{std::move(data_length.error())};
    }
    if (*data_length == 0) {
        auto packet_id = reader.varint();
        if (!packet_id) {
            return std::unexpected{std::move(packet_id.error())};
        }
        return DecodedFrame{*packet_id, reader.rest()};
    }
    if (*data_length < 0) {
        return make_error(ErrorCode::protocol, "negative uncompressed length");
    }
    const auto expected = static_cast<std::size_t>(*data_length);
    if (*data_length < threshold) {
        return make_error(ErrorCode::protocol,
                          std::format("compressed frame declares {} bytes below threshold {}", *data_length, threshold));
    }
    if (expected > static_cast<std::size_t>(kMaxFrameBytes)) {
        return make_error(ErrorCode::protocol,
                          std::format("inflated frame of {} bytes exceeds limit", expected));
    }
    auto inflated = inflate(reader.rest(), expected);
    if (!inflated) {
        return std::unexpected{std::move(inflated.error())};
    }
    scratch = std::move(*inflated);

    ByteReader inner{ByteSpan{scratch}};
    auto packet_id = inner.varint();
    if (!packet_id) {
        return std::unexpected{std::move(packet_id.error())};
    }
    // payload 指向 scratch，调用方须在下一次 decode_frame 之前用完
    return DecodedFrame{*packet_id, inner.rest()};
}

void encode_frame(Bytes& out, std::int32_t packet_id, ByteSpan fields, std::int32_t threshold) {
    Bytes body;
    body.reserve(fields.size() + 5);
    append_varint(body, packet_id);
    append(body, fields);

    if (threshold < 0) {
        append_varint(out, static_cast<std::int32_t>(body.size()));
        append(out, body);
        return;
    }

    if (static_cast<std::int32_t>(body.size()) < threshold) {
        append_varint(out, static_cast<std::int32_t>(body.size() + varint_size(0)));
        append_varint(out, 0);
        append(out, body);
        return;
    }

    auto compressed = deflate(body, kDefaultCompressionLevel);
    if (!compressed) {
        return;
    }
    Bytes header;
    append_varint(header, static_cast<std::int32_t>(body.size()));
    const auto payload_size = header.size() + compressed->size();
    append_varint(out, static_cast<std::int32_t>(payload_size));
    append(out, header);
    append(out, *compressed);
}

}
