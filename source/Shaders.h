#pragma once

#include <string>

/**
    The GLSL, as text.

    The walk runs on the CPU (Ant.cpp); the GPU only shows it. `kCommon` is a
    LIBRARY, not a shader: no #version, no main. Every pass is assembled as
    kVersion + kCommon + its piece, so the harness's checks run the text the
    plugin runs. tools/glslc.sh reassembles them for glslc.

    The state is RG32UI, one texel per cell: R holds the cell's colour (low 8
    bits) and whether an ant has ever stood there (bit 8), G the activity clock
    at its last visit. Read with texelFetch; floats appear only in the colours.

    Passes:

      sample     (Over, on Reseed) the clip's brightness at each cell's centre,
                 quantised into the rule's colours, read back for the walk
      composite  the grid box-filtered over each pixel, coloured by colour,
                 recent visits glowing, the ants marked; the clip (Over)
*/
namespace langton::shaders
{
extern const char* const kVersion;
extern const char* const kCommon;

extern const char* const kQuadVertex;
extern const char* const kSampleFragment;
extern const char* const kCompositeFragment;

/// kVersion + kCommon + the piece.
std::string Assemble( const char* piece );

} // namespace langton::shaders
