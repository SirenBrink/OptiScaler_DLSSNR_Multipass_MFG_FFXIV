#pragma once
#include <string>
enum class FGOutput{NoFG,FSRFG,DLSSG,XeFG};
struct State{std::string gameExe="ffxiv_dx11.exe";bool isHdrActive=false;static State& Instance(){static State s;return s;}};
