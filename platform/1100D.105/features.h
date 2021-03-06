#include "all_features.h"

#define FEATURE_LV_3RD_PARTY_FLASH
#define FEATURE_EYEFI_TRICKS

#undef FEATURE_AUTO_BURST_PICQ // not working on 600D, not sure about 1100D

// No MLU on the 1100D
#undef FEATURE_MLU
#undef FEATURE_MLU_HANDHELD

// Disable almost all audio stuff
#undef FEATURE_WAV_RECORDING
#undef FEATURE_FPS_WAV_RECORD
#undef FEATURE_BEEP
#undef FEATURE_VOICE_TAGS
#undef FEATURE_AUDIO_REMOTE_SHOT

// No DISP button
#undef FEATURE_OVERLAYS_IN_PLAYBACK_MODE
#undef FEATURE_ARROW_SHORTCUTS

#define FEATURE_INTERMEDIATE_ISO_PHOTO_DISPLAY
#undef FEATURE_SILENT_PIC // todo: port raw stuff
