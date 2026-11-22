// application.h — MonsterMesh app entry: display bring-up, input, main loop.
#pragma once

namespace mm {

class Application {
public:
    // Flags:
    //   --shots <dir>   headless: render every screen x demo state to PNGs, exit
    //   --screen <slug> start on a screen (menu party daycare breeding gyms battle wifi)
    //   --demo          force demo data (no daemon IPC)
    int run(int argc, char** argv);
};

} // namespace mm
