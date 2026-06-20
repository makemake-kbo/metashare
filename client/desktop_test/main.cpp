// MetaShare desktop test client.
//
// Mirrors what the Quest client must do, minus OpenXR: discover the streamer on
// the LAN (or use a host given on the CLI), connect over TCP, decode the H.264
// stream, and display it in a window. Used to validate the streamer end-to-end
// without Quest hardware.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL2/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include "protocol.hpp"

using namespace metashare;

namespace {

bool read_all(int fd, void* buf, std::size_t len) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::recv(fd, p + off, len - off, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Broadcast a discovery probe and wait for an offer. Fills host/port on success.
bool discover(std::string& host, std::uint16_t& port, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    proto::DiscoveryProbe probe{};
    std::memcpy(probe.magic, proto::kDiscoveryMagic, sizeof(probe.magic));
    probe.version = proto::kProtocolVersion;
    probe.client_caps = proto::kCapH264;

    std::fprintf(stderr, "[client] broadcasting discovery probe...\n");
    // Send to the global broadcast AND each interface's directed broadcast, so
    // discovery works on multi-homed dev boxes (docker/libvirt bridges, etc.)
    // where 255.255.255.255 only egresses one interface.
    auto send_to = [&](in_addr_t ip) {
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_addr.s_addr = ip;
        d.sin_port = htons(proto::kDiscoveryPort);
        ::sendto(fd, &probe, sizeof(probe), 0, reinterpret_cast<sockaddr*>(&d),
                 sizeof(d));
    };
    send_to(htonl(INADDR_BROADCAST));
    send_to(htonl(INADDR_LOOPBACK));  // same-machine streamer + client
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (ifaddrs* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_BROADCAST) || (p->ifa_flags & IFF_LOOPBACK))
                continue;
            if (p->ifa_broadaddr)
                send_to(reinterpret_cast<sockaddr_in*>(p->ifa_broadaddr)
                            ->sin_addr.s_addr);
        }
        ::freeifaddrs(ifa);
    }

    proto::DiscoveryOffer offer{};
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    ssize_t n = ::recvfrom(fd, &offer, sizeof(offer), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(offer))) return false;
    if (std::memcmp(offer.magic, proto::kDiscoveryMagic, sizeof(offer.magic)) !=
        0)
        return false;

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    host = ip;
    port = offer.stream_port;
    std::fprintf(stderr, "[client] found '%s' at %s:%u\n", offer.host_name, ip,
                 static_cast<unsigned>(port));
    return true;
}

int connect_tcp(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string p = std::to_string(port);
    if (::getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd >= 0) {
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host;
    std::uint16_t port = proto::kStreamPort;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "Usage: %s [--host <ip>] [--port <n>]\n"
                "  With no --host, discovers the streamer via UDP broadcast.\n",
                argv[0]);
            return 0;
        }
    }

    if (host.empty()) {
        if (!discover(host, port, 3000)) {
            std::fprintf(stderr,
                "[client] discovery failed; pass --host <ip> explicitly\n");
            return 1;
        }
    }

    int fd = connect_tcp(host, port);
    if (fd < 0) {
        std::fprintf(stderr, "[client] connect to %s:%u failed\n", host.c_str(),
                     static_cast<unsigned>(port));
        return 1;
    }

    proto::StreamHeader sh{};
    if (!read_all(fd, &sh, sizeof(sh)) ||
        std::memcmp(sh.magic, proto::kStreamMagic, sizeof(sh.magic)) != 0) {
        std::fprintf(stderr, "[client] bad stream header\n");
        ::close(fd);
        return 1;
    }
    std::fprintf(stderr, "[client] stream %ux%u codec=%u\n", sh.width, sh.height,
                 sh.codec);

    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* dctx = avcodec_alloc_context3(dec);
    if (!dec || !dctx || avcodec_open2(dctx, dec, nullptr) < 0) {
        std::fprintf(stderr, "[client] decoder init failed\n");
        return 1;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[client] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow(
        "MetaShare", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(sh.width), static_cast<int>(sh.height),
        SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren =
        SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(sh.width), static_cast<int>(sh.height));

    std::vector<std::uint8_t> payload;
    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                quit = true;

        proto::FrameHeader fh{};
        if (!read_all(fd, &fh, sizeof(fh))) break;
        payload.resize(fh.payload_size);
        if (!read_all(fd, payload.data(), fh.payload_size)) break;

        pkt->data = payload.data();
        pkt->size = static_cast<int>(payload.size());
        if (avcodec_send_packet(dctx, pkt) < 0) continue;
        while (avcodec_receive_frame(dctx, frame) == 0) {
            SDL_UpdateYUVTexture(tex, nullptr, frame->data[0],
                                 frame->linesize[0], frame->data[1],
                                 frame->linesize[1], frame->data[2],
                                 frame->linesize[2]);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        }
    }

    std::fprintf(stderr, "[client] stream closed\n");
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dctx);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    ::close(fd);
    return 0;
}
