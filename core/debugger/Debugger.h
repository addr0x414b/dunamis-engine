
#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <iostream>
#include <experimental/source_location>

using source_location = std::experimental::source_location;

class Debugger {
public:
    static void print(const char* message, bool error = false, source_location l = source_location::current());

    static void section(const char* sectionName);
    static void subSection(const char* subSectionName);
    static void subSubSection(const char* subSubSectionName);
};


#endif //DEBUGGER_H
