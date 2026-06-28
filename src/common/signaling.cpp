#include "signaling.hpp"

#include <sstream>
#include <string>
#include <string_view>

#include "base64.hpp"

namespace metashare::signal {

std::string serialize(const Message& m) {
    std::ostringstream os;
    switch (m.type) {
    case Type::kOffer: os << "OFFER " << b64::encode(m.body); break;
    case Type::kAnswer: os << "ANSWER " << b64::encode(m.body); break;
    case Type::kIce: os << "ICE " << b64::encode(m.body) << ' ' << m.mid; break;
    case Type::kOk: return "OK";
    case Type::kBye: return "BYE";
    }
    return os.str();
}

bool parse(std::string_view line, Message& out) {
    // Strip a trailing '\r' if the peer used CRLF.
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line == "OK") {
        out.type = Type::kOk;
        return true;
    }
    if (line == "BYE") {
        out.type = Type::kBye;
        return true;
    }

    auto sp = line.find(' ');
    if (sp == std::string_view::npos) return false;
    auto cmd = line.substr(0, sp);
    auto rest = line.substr(sp + 1);

    if (cmd == "OFFER" || cmd == "ANSWER") {
        bool ok = true;
        auto s = b64::decode_string(rest, &ok);
        if (!ok) return false;
        out.type = (cmd == "OFFER") ? Type::kOffer : Type::kAnswer;
        out.body = std::move(s);
        out.mid.clear();
        return true;
    }
    if (cmd == "ICE") {
        auto sp2 = rest.find(' ');
        if (sp2 == std::string_view::npos) return false;
        bool ok = true;
        auto cand = b64::decode_string(rest.substr(0, sp2), &ok);
        if (!ok) return false;
        out.type = Type::kIce;
        out.body = std::move(cand);
        out.mid = std::string(rest.substr(sp2 + 1));
        return true;
    }
    return false;
}

}  // namespace metashare::signal
