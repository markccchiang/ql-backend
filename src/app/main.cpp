/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file main.cpp
    \brief the ql-backend entry point
*/

#include "gateway/gateway.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    qlservice::Gateway::Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            options.port = std::atoi(argv[++i]);
        } else if (arg == "--host" && i + 1 < argc) {
            options.host = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: ql-backend [--host ADDR] [--port N]\n");
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
