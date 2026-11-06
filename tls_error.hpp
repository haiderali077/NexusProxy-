#pragma once

#include <string>
#include <string_view>

namespace nexus::security {

std::string_view openSslVersion();
std::string collectOpenSslErrors();

} // namespace nexus::security
