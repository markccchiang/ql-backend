/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file main.cpp
    \brief the ql-backend entry point
*/

#include "gateway/gateway.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    qlservice::Gateway::Options options;
    bool replacedOrigins = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            options.port = std::atoi(argv[++i]);
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
            options.maxConnections = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--max-sessions" && i + 1 < argc) {
            options.maxSessionsPerConnection = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--session-grace" && i + 1 < argc) {
            // Zero restores the rule this service had until now: a dropped
            // socket closes its sessions on the spot.
            options.resumeGrace = std::chrono::seconds(std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: ql-backend [--host ADDR] [--port N]\n"
                        "                  [--allow-origin URL]... | [--any-origin]\n"
                        "                  [--max-connections N] [--max-sessions N]\n"
                        "                  [--session-grace SECONDS]\n"
                        "\n"
                        "A session outlives its socket by --session-grace seconds (60 by\n"
                        "default, 0 to turn it off), so a client that loses its connection can\n"
                        "take the session back with ResumeSession and the token it was given.\n"
                        "Whatever was running keeps running, and its result is held.\n"
                        "\n"
                        "A browser is not bound by the same-origin policy when it opens a\n"
                        "WebSocket, so an Origin that is present must be on the allowed list.\n"
                        "A request with no Origin -- a script, a proxy that already checked --\n"
                        "is not affected.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    try {
        qlservice::Gateway gateway(options);
        std::printf("[qlservice] listening on ws://%s:%d\n", options.host.c_str(), options.port);
        std::fflush(stdout);
        if (!gateway.run()) {
            std::fprintf(stderr, "[qlservice] could not listen on port %d\n", options.port);
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[qlservice] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
