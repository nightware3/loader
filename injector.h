#pragma once

#include <windows.h>
#include <iostream>
#include <fstream>
#include <TlHelp32.h>
#include <stdio.h>
#include <string>

namespace Injector {
	bool Inject(const char* processName, const char* dllPath);
}