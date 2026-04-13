#ifndef XDR_AUDIO_BRIDGE_DEBUG_H_
#define XDR_AUDIO_BRIDGE_DEBUG_H_

/* Opens a Windows console (AllocConsole) and starts a 500 ms polling
   timer that prints one line per tick with every audio bridge counter,
   plus separate lines for transitions (peak drop/recover, restarts,
   volume/mute changes). Safe to call multiple times — the console is
   created lazily and the timer only runs once.

   Temporary helper for diagnosing the silent-after-a-while problem; can
   be removed once the root cause is identified. */
void audio_bridge_debug_show(void);

#endif
