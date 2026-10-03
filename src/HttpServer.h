#pragma once

#include "CommandLine.h"
#include <optional>
#include "preflight/LuaPreflight.h"

namespace sipi {

/**
 * Serve health JSON, hello HTML, and an IIIF metadata/geometry catch-all on loopback HTTP.
 * GET/HEAD are accepted; other methods receive 405. Invalid IIIF paths receive
 * 400. Optional preflight runs off the I/O thread: deny=403, failure=500,
 * missing source=404; invalid geometry=400. Supported TIFF crops return JPEG;
 * unsupported operations/layouts return 501. Information requests return JSON. Throws SourceError for an invalid configured root.
 * Blocks in Drogon's event loop until shutdown (SIGINT/SIGTERM). Call once from
 * main after startup configuration. The listener is local-only; HTTPS belongs
 * to the reverse proxy. No cache, socket streaming, or static files are served.
 */
void run_http_server(const ServerConfig& config,
                     std::optional<preflight::LuaPreflight> policy = std::nullopt);

} // namespace sipi
