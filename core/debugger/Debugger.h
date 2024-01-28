//
// Created by alex on 1/28/24.
//

#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <iostream>
#include <experimental/source_location>

using source_location = std::experimental::source_location;

class Debugger {
public:
    static void print(const char* message, bool error = false, source_location l = source_location::current());

    static void section(const char* sectionName);
};


#endif //DEBUGGER_H
