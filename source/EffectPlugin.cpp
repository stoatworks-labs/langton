#include "Langton.h"

/**
    The effect: the clip is the ground. Its brightness, quantised into the
    rule's colours, is what the ants read as they walk, and they repaint the
    picture as they go.

    See SourcePlugin.cpp for why this file is listed in its own target.
*/
namespace
{
class LangtonEffect : public langton::LangtonPlugin
{
public:
	LangtonEffect() :
		LangtonPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< LangtonEffect >,// Create method
	"LA02",                        // Plugin unique ID of maximum length 4
	"SW Langton Over",             // Plugin name
	2,                             // API major version number
	1,                             // API minor version number
	0,                             // Plugin major version number
	1,                             // Plugin minor version number
	FF_EFFECT,                     // Plugin type
	"Langton's ant walking your clip: its brightness, in the rule's colours, is the ground the ants read and "
	"repaint as they go. Reseed takes the clip again; Mix brings it back.",
	"Langton FFGL effect"          // About
);

extern "C" const char* LangtonEffectBuildStamp()
{
	return "langton " LANGTON_VERSION " effect, built " __DATE__ " " __TIME__;
}
