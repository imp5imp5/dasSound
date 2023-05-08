#ifndef volume_mixer_h
#define volume_mixer_h

// volume mixer
// i keep it in ma_ style, and potentially will make a PR to miniaudio at some point
// #include <miniaudio.h>

struct ma_volume_mixer {
    float volume;
    float dvolume;
    float tvolume;
    uint32_t nChannels;
};

void ma_volume_mixer_init ( ma_volume_mixer * mixer, uint32_t nChannels );
void ma_volume_mixer_uninit ( ma_volume_mixer * mixer );
void ma_volume_mixer_set_channels ( ma_volume_mixer * mixer, uint32_t nChannels );
void ma_volume_mixer_set_volume ( ma_volume_mixer * mixer, float volume );
void ma_volume_mixer_set_volume_over_time ( ma_volume_mixer * mixer, float volume, uint64_t nFrames );
void ma_volume_mixer_process_pcm_frames ( ma_volume_mixer * mixer, float * InFrames, float * OutFrames, uint64_t nFrames );

#ifdef MINIAUDIO_IMPLEMENTATION

void ma_volume_mixer_init ( ma_volume_mixer * mixer, uint32_t nChannels ) {
    mixer->volume = 1.0f;
    mixer->dvolume = 0.0f;
    mixer->tvolume = 1.0f;
    mixer->nChannels = nChannels;
}

void ma_volume_mixer_uninit ( ma_volume_mixer * ) {
    // do nothing
}

void ma_volume_mixer_set_channels ( ma_volume_mixer * mixer, uint32_t nChannels ) {
    mixer->nChannels = nChannels;
}

void ma_volume_mixer_set_volume ( ma_volume_mixer * mixer, float volume ) {
    mixer->volume = volume;
    mixer->dvolume = 0.0f;
    mixer->tvolume = volume;
}

void ma_volume_mixer_set_volume_over_time ( ma_volume_mixer * mixer, float volume, uint64_t nFrames ) {
    mixer->dvolume = (volume - mixer->volume) / float(nFrames);
    mixer->tvolume = volume;
}

void ma_volume_mixer_process_pcm_frames_linear ( ma_volume_mixer * mixer, float * InFrames, float * OutFrames, uint64_t nFrames ) {
    float volume = mixer->volume;
    uint32_t nChannels = mixer->nChannels;
    if ( nChannels==1 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i] += InFrames[i] * volume;
        }
    } else if ( nChannels==2 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i*2+0] += InFrames[i*2+0] * volume;
            OutFrames[i*2+1] += InFrames[i*2+1] * volume;
        }
    } else if ( nChannels==4 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i*4+0] += InFrames[i*4+0] * volume;
            OutFrames[i*4+1] += InFrames[i*4+1] * volume;
            OutFrames[i*4+2] += InFrames[i*4+2] * volume;
            OutFrames[i*4+3] += InFrames[i*4+3] * volume;
        }
    } else {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            for ( uint32_t j=0; j!=nChannels; ++j ) {
                OutFrames[i*nChannels+j] += InFrames[i*nChannels+j] * volume;
            }
        }
    }
}

void ma_volume_mixer_process_pcm_frames_descending ( ma_volume_mixer * mixer, float * InFrames, float * OutFrames, uint64_t nFrames ) {
    uint32_t nChannels = mixer->nChannels;
    float volume = mixer->volume;
    float dvolume = mixer->dvolume;
    float tvolume = mixer->tvolume;
    if ( nChannels==1 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i] += InFrames[i] * volume;
            volume = ma_max(volume+dvolume,tvolume);
        }
    } else if ( nChannels==2 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i*2+0] += InFrames[i*2+0] * volume;
            OutFrames[i*2+1] += InFrames[i*2+1] * volume;
            volume = ma_max(volume+dvolume,tvolume);
        }
    } else if ( nChannels==4 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i*4+0] += InFrames[i*4+0] * volume;
            OutFrames[i*4+1] += InFrames[i*4+1] * volume;
            OutFrames[i*4+2] += InFrames[i*4+2] * volume;
            OutFrames[i*4+3] += InFrames[i*4+3] * volume;
            volume = ma_max(volume+dvolume,tvolume);
        }
    } else {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            for ( uint32_t j=0; j!=nChannels; ++j ) {
                OutFrames[i*nChannels+j] += InFrames[i*nChannels+j] * volume;
                volume = ma_max(volume+dvolume,tvolume);
            }
        }
    }
    mixer->volume = volume;
    mixer->dvolume = volume==tvolume ? 0.0f : dvolume;
}

void ma_volume_mixer_process_pcm_frames_ascending ( ma_volume_mixer * mixer, float * InFrames, float * OutFrames, uint64_t nFrames ) {
    uint32_t nChannels = mixer->nChannels;
    float volume = mixer->volume;
    float dvolume = mixer->dvolume;
    float tvolume = mixer->tvolume;
    if ( nChannels==1 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i] += InFrames[i] * volume;
            volume = ma_min(volume+dvolume,tvolume);
        }
    } else if ( nChannels==2 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i*2+0] += InFrames[i*2+0] * volume;
            OutFrames[i*2+1] += InFrames[i*2+1] * volume;
            volume = ma_min(volume+dvolume,tvolume);
        }
    } else if ( nChannels==4 ) {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            OutFrames[i*4+0] += InFrames[i*4+0] * volume;
            OutFrames[i*4+1] += InFrames[i*4+1] * volume;
            OutFrames[i*4+2] += InFrames[i*4+2] * volume;
            OutFrames[i*4+3] += InFrames[i*4+3] * volume;
            volume = ma_min(volume+dvolume,tvolume);
        }
    } else {
        for ( uint64_t i=0; i!=nFrames; ++i ) {
            for ( uint32_t j=0; j!=nChannels; ++j ) {
                OutFrames[i*nChannels+j] += InFrames[i*nChannels+j] * volume;
                volume = ma_min(volume+dvolume,tvolume);
            }
        }
    }
    mixer->volume = volume;
    mixer->dvolume = volume==tvolume ? 0.0f : dvolume;
}

void ma_volume_mixer_process_pcm_frames ( ma_volume_mixer * mixer, float * InFrames, float * OutFrames, uint64_t nFrames ) {
    if ( mixer->dvolume==0.0f ) {
        ma_volume_mixer_process_pcm_frames_linear(mixer,InFrames,OutFrames,nFrames);
    } else if ( mixer->dvolume<0.0f ) {
        ma_volume_mixer_process_pcm_frames_descending(mixer,InFrames,OutFrames,nFrames);
    } else {
        ma_volume_mixer_process_pcm_frames_ascending(mixer,InFrames,OutFrames,nFrames);
    }
}

#endif

#endif
