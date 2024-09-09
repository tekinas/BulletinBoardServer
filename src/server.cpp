#include "server.hpp"

int main(int argc, char const *argv[]) {
    if (argc > 2) {
        std::print("error: more than one command-line argument specified\n"
                   "usage: {} <bbserf.conf>\n",
                   argv[0]);
        return EXIT_FAILURE;
    }
    auto const configFile = argc == 1 ? "bbserv.conf" : argv[1];
    Server server{configFile};
    server.start();
}
