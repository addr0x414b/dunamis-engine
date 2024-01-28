// Main entry point to the Dunamis Engine

#include <iostream>

#include "core/Dunamis.h"
#include "core/debugger/Debugger.h"

int main() {

    Debugger::section("Main");
    Debugger::print("This is the error message bruh");
    Debugger::section("Main");

    Dunamis engine;


}