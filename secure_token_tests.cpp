#include "secure_token.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main() {
    const std::string expectedText = "0123456789abcdef0123456789abcdef";
    std::string source = expectedText;
    nexus::security::SecureToken token{source};
    if (!source.empty() || token.size() != expectedText.size()) {
        std::cerr << "Secure token did not take over the source bytes"
                  << std::endl;
        return 1;
    }

    const std::vector<std::uint8_t> expected(expectedText.begin(),
                                             expectedText.end());
    if (!token.matches(expected)) {
        std::cerr << "Matching token was rejected" << std::endl;
        return 1;
    }

    auto wrong = expected;
    wrong.back() ^= 0x01;
    if (token.matches(wrong) || token.matches(std::span{wrong}.first(8))) {
        std::cerr << "Incorrect token was accepted" << std::endl;
        return 1;
    }

    nexus::security::SecureToken moved{std::move(token)};
    if (!token.empty() || !moved.matches(expected)) {
        std::cerr << "Moving a secure token lost or duplicated ownership"
                  << std::endl;
        return 1;
    }

    std::cout << "Secure token ownership and comparison passed" << std::endl;
    return 0;
}
