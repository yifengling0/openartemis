#pragma once
// SDL3 render header shim for the SDL2 host line.
//
// The engine's render path never touches SDL_Render directly (render/backend.h
// is the seam); the test host (app_test_drive.cpp) only carries the include for
// historical reasons. The bridge maps every SDL3 name it actually needs onto
// SDL2 through oa_sdl2_compat.h, so this shim just forwards there — which lets
// the test build compile on the VintagePomelo/OHOS (SDL2) line as well.
#include "../oa_sdl2_compat.h"
