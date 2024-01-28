
#include "Debugger.h"

 void Debugger::print(const char *message, bool error, source_location l) {
    if (error) {
        throw std::runtime_error(std::string(source_location::current().file_name()) + ": " + std::to_string(l.line()) + '\n' + message);
    } else {
        std::cout << message << std::endl;
    }
}

void Debugger::section(const char *sectionName) {
    std::cout << "--------------------------------------- " << sectionName << " ---------------------------------------" << std::endl;
}
