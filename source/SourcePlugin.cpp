#include "Langton.h"

/**
    The source: Langton's ant (and Turk and Propp's many-coloured ants) on an
    empty grid or a soup.

    Listed directly in the LangtonSource target, not in langton_core: both
    plugins share the class and not the `CFFGLPluginInfo` below, and putting
    either registration in the shared library would register both plugins into
    both bundles. The core is an OBJECT library because this registers itself
    from a file-scope constructor nothing references (see CMakeLists.txt).

    `SW Langton` is ten characters; the FFGL name field is char[ 16 ] and not
    null-terminated. `oxbow probe` reads it back the way a host does.
*/
namespace
{
class LangtonSource : public langton::LangtonPlugin
{
public:
	LangtonSource() :
		LangtonPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< LangtonSource >,// Create method
	"LA01",                        // Plugin unique ID of maximum length 4
	"SW Langton",                  // Plugin name
	2,                             // API major version number
	1,                             // API minor version number
	0,                             // Plugin major version number
	1,                             // Plugin minor version number
	FF_SOURCE,                     // Plugin type
	"Langton's ant. It turns right on a white cell and left on a black one, flips the cell and steps forward, and "
	"from that alone come ten thousand steps of chaos and then a highway, for ever. Twelve-colour ants too, several "
	"at once, and time can run backwards: the walk is exactly reversible.",
	"Langton FFGL source"          // About
);

extern "C" const char* LangtonSourceBuildStamp()
{
	return "langton " LANGTON_VERSION " source, built " __DATE__ " " __TIME__;
}
