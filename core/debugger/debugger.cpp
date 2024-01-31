#include "debugger.h"

void Debugger::print(const char *message, bool error, source_location l) {
    if (error) {
        throw std::runtime_error(std::string(l.file_name()) + ": " +
                                 std::to_string(l.line()) + '\n' + message);
    } else {
        std::cout << message << std::endl;
    }
}

void Debugger::section(const char *sectionName) {
    std::cout << "--------------------------------------- " << sectionName
              << std::endl;
}

void Debugger::subSection(const char *subSectionName) {
    std::cout << "----------------------------- " << subSectionName
              << std::endl;
}

void Debugger::subSubSection(const char *subSubSectionName) {
    std::cout << "-------------------- " << subSubSectionName << std::endl;
}