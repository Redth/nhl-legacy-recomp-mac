// Game-side definitions for the present_grade_* cvars.
//
// The color-grade post-process (overlay "Lighting / Color Grade", commit
// 768c484) dispatches a compute pass in the SDK's Vulkan present path, and the
// original dev's SDK tree defined these cvars there — but that SDK-side code
// was never captured in the title patch (sdk/rexglue-vulkan-nhl-legacy-
// bd9b519.patch has no present_grade hunks), so a from-public-source runtime
// build leaves them undefined and the game fails to link.
//
// Defining them here keeps the game linking and the overlay section
// functional as UI; the values just aren't consumed by the rebuilt runtime,
// so the grade pass is a NO-OP until it is reimplemented SDK-side (the
// compute shader already lives in renderer/shaders/nhl_grade.comp /
// nhl_grade_cs.h). Delete this TU when that lands to avoid duplicate
// definitions against an SDK that defines them again.

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(present_grade_enable, false, "NHL",
                    "Enable the present-time color-grade pass (no-op until the "
                    "SDK-side pass is reimplemented)");
REXCVAR_DEFINE_DOUBLE(present_grade_exposure, 0.0, "NHL",
                      "Color grade: exposure (stops)");
REXCVAR_DEFINE_DOUBLE(present_grade_contrast, 1.0, "NHL",
                      "Color grade: contrast");
REXCVAR_DEFINE_DOUBLE(present_grade_saturation, 1.0, "NHL",
                      "Color grade: saturation");
REXCVAR_DEFINE_DOUBLE(present_grade_brightness, 0.0, "NHL",
                      "Color grade: brightness offset");
REXCVAR_DEFINE_DOUBLE(present_grade_temperature, 0.0, "NHL",
                      "Color grade: white-balance temperature");
REXCVAR_DEFINE_DOUBLE(present_grade_tint, 0.0, "NHL",
                      "Color grade: white-balance tint");
REXCVAR_DEFINE_DOUBLE(present_grade_tonemap, 0.0, "NHL",
                      "Color grade: filmic tone-map strength");
