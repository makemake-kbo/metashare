// Tiny RFC 4648 base64 codec — we only need it for the signaling channel,
// where SDP offer/answer strings and ICE candidates are wrapped so the
// line-based framing protocol can stay simple (no JSON-escaping required).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace metashare::b64 {

inline constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string encode(const std::uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < n) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < n) v |= static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < n ? kAlphabet[v & 0x3F] : '=');
    }
    return out;
}

inline std::string encode(const std::string& s) {
    return encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// Decode to bytes. Sets *ok to false on malformed input.
inline std::vector<std::uint8_t> decode(std::string_view in,
                                        bool* ok = nullptr) {
    std::int8_t table[256];
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i)
        table[static_cast<unsigned char>(kAlphabet[i])] =
            static_cast<std::int8_t>(i);

    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        auto d = table[static_cast<unsigned char>(c)];
        if (d < 0) {
            if (ok) *ok = false;
            return {};
        }
        v = (v << 6) | static_cast<std::uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((v >> bits) & 0xFF));
        }
    }
    if (ok) *ok = true;
    return out;
}

inline std::string decode_string(std::string_view in, bool* ok = nullptr) {
    auto bytes = decode(in, ok);
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

}  // namespace metashare::b64
