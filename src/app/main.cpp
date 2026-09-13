/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file main.cpp
    \brief the ql-backend entry point
*/

#include "gateway/gateway.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

namespace {

    //! Reads a whole number in [low, high] or says why not and exits 2.
    /*! atoi read "--port abc" as port 0 and "--max-sessions -1" as the
        largest size there is, and the daemon started on either. A flag that
        does not parse is the operator's mistake, and it is reported as one.
    */
    long long parseCount(const char* flag, const char* text, long long low, long long high) {
        long long value = 0;
        const char* end = text + std::strlen(text);
        const auto [ptr, ec] = std::from_chars(text, end, value);
        if (ec != std::errc() || ptr != end || value < low || value > high) {
            std::fprintf(stderr, "%s takes a whole number from %lld to %lld, not '%s'\n", flag,
                         low, high, text);
            std::exit(2);
        }
        return value;
    }

    //! Whether an address reaches this machine and nothing else.
    /*! A string comparison rather than a resolve: the guard below has to be
        explainable in the message it prints, and "the address you gave is not
        one of these three" is a rule an operator can act on. Anything
        cleverer would refuse or allow on a lookup nobody can see.
    */
    bool isLoopback(const std::string& host) {
        return host == "127.0.0.1" || host == "::1" || host == "localhost";
    }

    //! 128 bits from the platform's entropy source, as hex.
    std::string mintToken() {
        std::random_device entropy;
        std::string out;
        out.reserve(32);
        for (int i = 0; i < 4; ++i) {
            char buf[9];
            std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(entropy()));
            out += buf;
        }
        return out;
    }

    //! The shared secret, read from PATH or minted into it on first use.
    /*! A file and not a flag. Arguments are world-readable through ps, so
        `--token abc` would publish the secret to precisely the local
        processes it exists to keep out, and an environment variable is only
        a little better. The file is created 0600 and refused if it is
        readable by anyone else, because a secret that is not is not one.
    */
    std::string tokenFromFile(const std::string& path) {
        struct stat info{};
        if (::stat(path.c_str(), &info) == 0) {
            // Before the permission check, or a directory fails it: every
            // directory carries the group and other bits this looks for, so
            // the answer would be "chmod 600 it" -- useless advice about the
            // wrong problem, and bad advice about a directory.
            if (!S_ISREG(info.st_mode)) {
                std::fprintf(stderr,
                             "%s is not a regular file; --token-file names the file the "
                             "secret is kept in, not the directory it sits in\n",
                             path.c_str());
                std::exit(2);
            }
            if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
                std::fprintf(stderr,
                             "%s can be read by other users, so it is not a secret; "
                             "chmod 600 it\n",
                             path.c_str());
                std::exit(2);
            }
            std::ifstream in(path);
            std::string token;
            std::getline(in, token);
            while (!token.empty() &&
                   std::isspace(static_cast<unsigned char>(token.back())) != 0)
                token.pop_back();
            if (token.empty()) {
                std::fprintf(stderr, "%s holds no token\n", path.c_str());
                std::exit(2);
            }
            return token;
        }

        // O_EXCL: the file was absent a moment ago, and creating it is the
        // one moment where a race would hand the secret to whoever won.
        const auto token = mintToken();
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            std::fprintf(stderr, "could not create %s: %s\n", path.c_str(), std::strerror(errno));
            std::exit(2);
        }
        const std::string line = token + "\n";
        const bool written = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
        ::close(fd);
        if (!written) {
            std::fprintf(stderr, "could not write %s\n", path.c_str());
            std::exit(2);
        }
        std::printf("[ql-backend] minted a token in %s\n", path.c_str());
        return token;
    }

}

int main(int argc, char** argv) {
    qlservice::Gateway::Options options;
    bool replacedOrigins = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            options.port = static_cast<int>(parseCount("--port", argv[++i], 1, 65535));
        } else if (arg == "--host" && i + 1 < argc) {
            options.host = argv[++i];
        } else if (arg == "--allow-origin" && i + 1 < argc) {
            // Replaces the defaults on first use rather than adding to them:
            // a deployment that names its own origins does not also want the
            // development server's.
            if (!replacedOrigins) {
                options.allowedOrigins.clear();
                replacedOrigins = true;
            }
            options.allowedOrigins.emplace_back(argv[++i]);
        } else if (arg == "--any-origin") {
            // For a deployment behind a proxy that already checks. Named so it
            // reads as a decision in a command line rather than a default.
            options.allowedOrigins.clear();
            replacedOrigins = true;
        } else if (arg == "--max-connections" && i + 1 < argc) {
            options.maxConnections =
                static_cast<std::size_t>(parseCount("--max-connections", argv[++i], 1, 1 << 20));
        } else if (arg == "--max-sessions" && i + 1 < argc) {
            options.maxSessionsPerConnection =
                static_cast<std::size_t>(parseCount("--max-sessions", argv[++i], 1, 1 << 20));
        } else if (arg == "--token-file" && i + 1 < argc) {
            options.authToken = tokenFromFile(argv[++i]);
        } else if (arg == "--session-grace" && i + 1 < argc) {
            // Zero restores the rule this service had until now: a dropped
            // socket closes its sessions on the spot.
            options.resumeGrace =
                std::chrono::seconds(parseCount("--session-grace", argv[++i], 0, 86400));
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: ql-backend [--host ADDR] [--port N]\n"
                        "                  [--allow-origin URL]... | [--any-origin]\n"
                        "                  [--max-connections N] [--max-sessions N]\n"
                        "                  [--session-grace SECONDS] [--token-file FILE]\n"
                        "\n"
                        "A session outlives its socket by --session-grace seconds (60 by\n"
                        "default, 0 to turn it off), so a client that loses its connection can\n"
                        "take the session back with ResumeSession and the token it was given.\n"
                        "Whatever was running keeps running, and its result is held.\n"
                        "\n"
                        "A browser is not bound by the same-origin policy when it opens a\n"
                        "WebSocket, so an Origin that is present must be on the allowed list.\n"
                        "A request with no Origin -- a script, a proxy that already checked --\n"
                        "is not affected.\n"
                        "\n"
                        "--token-file names the file a shared secret is kept in, not the\n"
                        "directory it sits in. The file is read if it exists and minted at 0600\n"
                        "if it does not, and it is refused if anyone but its owner can read it.\n"
                        "Every client then presents the secret at the upgrade: an\n"
                        "Authorization: Bearer header, or a `token.<secret>` entry in the\n"
                        "WebSocket subprotocol list for a browser, which cannot set headers.\n"
                        "Without a token this service listens on loopback only: an address that\n"
                        "is not loopback and no token is refused rather than served.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    // Fail closed. Everything else here is a door against a page, and none of
    // it is a boundary against the network: reaching a routable address with
    // no token is how an unauthenticated pricing engine ends up answering the
    // office, and it is far likelier to be a mistake than a decision. Said
    // rather than assumed, with the two ways out named.
    if (!isLoopback(options.host) && options.authToken.empty()) {
        std::fprintf(stderr,
                     "refusing to listen on %s without a token: this service has no\n"
                     "authentication of its own, and that address is reachable from the\n"
                     "network. Give it --token-file FILE, or leave it on loopback and put a\n"
                     "proxy that terminates TLS and authenticates in front of it.\n",
                     options.host.c_str());
        return 2;
    }

    try {
        qlservice::Gateway gateway(options);
        std::printf("[ql-backend] listening on ws://%s:%d%s\n", options.host.c_str(), options.port,
                    options.authToken.empty() ? "" : " (a token is required)");
        std::fflush(stdout);
        if (!gateway.run()) {
            std::fprintf(stderr, "[ql-backend] could not listen on port %d\n", options.port);
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ql-backend] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
