#include "oreslang/frontend/Parser.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: parser_gate <source> <expected-diagnostic>\n";
        return 64;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "cannot open fixture\n";
        return 65;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    try {
        auto program = oreslang::parse_program(buffer.str());
        (void)program;
        std::cerr << "unexpected native admission\n";
        return 1;
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (message.find(argv[2]) == std::string::npos) {
            std::cerr << "wrong diagnostic: " << message << "\n";
            return 2;
        }
        std::cout << "EXPECTED_REJECTION_PASS " << argv[1] << "\n";
        return 0;
    }
}
