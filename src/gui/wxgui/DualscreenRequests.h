#pragma once

class MainWindow;

// What Dualscreen's menu asks of Cemu itself -- the game's graphics packs,
// Cemu's own options, an amiibo scanned -- answered on `window`'s thread (the
// UI thread), as Cemu's own windows would. Install before a game starts, which
// is when Dualscreen is told what can be asked; uninstall before `window` goes.
void DualscreenRequests_Install(MainWindow* window);
void DualscreenRequests_Uninstall();
