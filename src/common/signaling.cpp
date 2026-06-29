#include "signaling.hpp"

#include <sstream>
#include <string>
#include <string_view>

namespace metashare::signal {

std::string serialize(const Message& m) {
    std::ostringstream os;
    switch (m.type) {
    case Type::kHello: os << "HELLO " << m.body; break;
    case Type::kReady: os << "READY " << m.body; break;
    case Type::kStart: return "START";
    case Type::kBye: return "BYE";
    }
    return os.str();
}

bool parse(std::string_view line, Message& out) {
    // Strip a trailing '\r' if the peer used CRLF.
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line == "START") {
        out.type = Type::kStart;
        return true;
    }
    if (line == "BYE") {
        out.type = Type::kBye;
        return true;
    }

    auto sp = line.find(' ');
    if (sp == std::string_view::npos) return false;
    auto cmd = line.substr(0, sp);
    // Compact JSON bodies carry no newlines, so the rest of the line is the
    // whole body. Keep the exact bytes (no base64 round-trip).
    auto rest = line.substr(sp + 1);

    if (cmd == "HELLO") {
        out.type = Type::kHello;
        out.body = std::string(rest);
        return true;
    }
    if (cmd == "READY") {
        out.type = Type::kReady;
        out.body = std::string(rest);
        return true;
    }
    return false;
}

}  // namespace metashare::signal
