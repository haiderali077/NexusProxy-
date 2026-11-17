#pragma once

namespace nexus::process {

// Signal handlers only set a flag; the normal event loop performs the cleanup.
bool installShutdownHandlers();
bool shutdownRequested();

} // namespace nexus::process
