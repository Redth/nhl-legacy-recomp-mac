// VP6 host-decode bridge (see vp6_bridge.cpp). Called from the movie-player
// hooks in diag_hooks.cpp; both no-op unless NHL_VP6_BRIDGE=1.
#pragma once

#include <cstdint>

// Movie setup (sub_8277ABB8): consume NHL_VP6_LAST_OPEN and spawn the decoder.
void Vp6BridgeOnMovieSetup();

// Plane publish (sub_8277CC98, after the guest copy): overwrite the ring
// slot's planes with the next host-decoded frame. Pointers are HOST pointers
// to the guest planes; pitches in bytes.
void Vp6BridgePublish(uint8_t* y, uint8_t* u, uint8_t* v, uint32_t w,
                      uint32_t h, uint32_t y_pitch, uint32_t c_pitch);
