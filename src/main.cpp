#include "cipher/cipher_core.hpp"

#include <iostream>
#include <string>

int main() {
    cipher::CipherCore core;

    std::cout << "CIPHER v" << cipher::CipherCore::version() << '\n';
    std::cout << "Contextual Intelligence Platform for Home, Enterprise & Research\n";
    std::cout << "Type 'help' to begin. This initial build runs locally and has no AI runtime.\n\n";

    std::string input;
    while (true) {
        std::cout << "CIPHER> ";
        if (!std::getline(std::cin, input)) {
            std::cout << "\nInput closed. CIPHER session ended.\n";
            break;
        }

        const cipher::CommandResponse response = core.execute(input);
        if (!response.text.empty()) {
            std::cout << response.text << '\n';
        }
        if (response.exit_requested) {
            break;
        }
    }

    return 0;
}
