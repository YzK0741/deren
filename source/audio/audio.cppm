// deren.audio: the audio module's public module - a facade over its partitions.
//
// Nothing outside this module imports the partitions, so they are partitions rather than
// separate modules, and this unit is the only name a consumer needs: import deren.audio.
export module deren.audio;

export import deren.vstd;

export import :declarations;
export import :audio_load;

export import :playback;
