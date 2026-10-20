#include "mcpp/cli_entry.hpp"

int main(const int argc, char** argv) {
    return mcpp::run_cli(mcpp::CliFrontend::manager, argc, argv);
}
