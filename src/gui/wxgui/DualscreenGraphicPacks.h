#pragma once

class wxEvtHandler;

// Dualscreen's Graphics Packs page: lists the running game's packs for it and
// makes the changes it asks for, on `owner`'s thread (the UI thread), as
// Cemu's own Graphic Packs window would. Uninstall before `owner` goes.
void DualscreenGraphicPacks_Install(wxEvtHandler* owner);
void DualscreenGraphicPacks_Uninstall();
