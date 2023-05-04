#include "daScript/misc/platform.h"

#include "dasSound.h"

#include "daScript/ast/ast.h"
#include "daScript/ast/ast_interop.h"
#include "daScript/ast/ast_typefactory_bind.h"
#include "daScript/simulate/interop.h"
#include "daScript/simulate/simulate_visit_op.h"

#include <opl3.h>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <array>

#ifndef _MSC_VER
#define stricmp strcasecmp
#endif

MAKE_TYPE_FACTORY(PlayingSoundHandle, das::sound::PlayingSoundHandle)
MAKE_TYPE_FACTORY(PcmSound, das::sound::PcmSound)

MAKE_TYPE_FACTORY(Opl3Chip, opl3_chip)


namespace das {


template <>
struct cast <sound::PlayingSoundHandle>
{
  static __forceinline sound::PlayingSoundHandle to(vec4f x)
  {
    sound::PlayingSoundHandle ps;
    ps.handle = cast<uint32_t>::to(x);
    return ps;
  }

  static __forceinline vec4f from(sound::PlayingSoundHandle x)
  {
    return cast<uint32_t>::from(x.handle);
  }
};




#define VERY_BIG_NUMBER 2147440000
#define G_UNUSED(x) ((void)(x))

#define OUTPUT_SAMPLE_RATE 48000
#define OUTPUT_CHANNELS 2

#define MAX_PLAYING_SOUNDS 128 // power of 2
#define PLAYING_SOUNDS_MASK (MAX_PLAYING_SOUNDS - 1)
#define ONE_DIV_256 (1.f / 256)
#define ONE_DIV_512 (1.f / 512)

static int current_frame = 0;  // !!!!!!!!!!!!!!!!!



namespace sound
{

struct DasboxDebugInfo
{
  char name[40];
  int creationFrame;

  DasboxDebugInfo()
  {
    reset();
  }

  void reset()
  {
    name[0] = 0;
    name[sizeof(name) - 1] = 0;
    creationFrame = 0; // current_frame; !!!!!!!!!!!!!!!!!!!!!!!!
  }
};

template <class T> inline T sign(T x)
{
  return (x < 0) ? T(-1) : ((x > 0) ? T(1) : 0);
}

template <typename T> inline T clamp(T t, const T min_val, const T max_val)
{
  if (t < min_val)
    t = min_val;
  if (t <= max_val)
    return t;
  return max_val;
}

template <typename T> inline T lerp(T a, T b, float t)
{
  return (b - a) * t + a;
}


int playing_sound_count = 0;
int64_t total_samples_played = 0;
volatile double total_time_played = 0.0;

static mutex sound_cs;
static volatile bool sound_cs_manual_entered = false;

static float master_volume = 1.0f;

static das_hash_set<float *> sound_data_pointers;
static das_hash_set<DasboxDebugInfo *> dbg_pointers;
static uint64_t memory_used = 0;

int get_total_sound_count()
{
  return int(sound_data_pointers.size());
}

int get_playing_sound_count()
{
  return int(playing_sound_count);
}


void PcmSound::newData(size_t size)
{
  memoryUsed = unsigned(size);
  data = new float[(size + 3) / sizeof(float)];
  sound_data_pointers.insert(data);
  memory_used += memoryUsed;
}

void PcmSound::deleteData()
{
  memory_used -= memoryUsed;

  sound_data_pointers.erase(data);
  delete[] data;
  data = nullptr;

  dbg_pointers.erase(dbg);
  delete dbg;
  dbg = nullptr;
}


PcmSound::PcmSound()
{
  memoryUsed = 0;
  frequency = 44100;
  samples = 0;
  channels = 1;
  data = nullptr;
  dbg = nullptr;
}

PcmSound::PcmSound(const PcmSound & b)
{
  memoryUsed = 0;
  frequency = b.frequency;
  samples = b.samples;
  channels = b.channels;
  newData(getDataMemorySize());
  memcpy(data, b.data, getDataMemorySize());
  dbg = b.dbg ? new DasboxDebugInfo(*b.dbg) : nullptr;
  if (dbg)
    dbg->creationFrame = current_frame;
}


struct PlayingSound
{
  const PcmSound * sound;
  double pos;      // in samples
  double startPos; // in samples
  double stopPos;  // in samples
  float pitch;
  float volume;
  float pan;
  float volumeL;
  float volumeR;
  float volumeTrendL;
  float volumeTrendR;
  double timeToStart; // in seconds
  int channels;
  int version;
  bool loop;
  bool stopMode;
  bool waitingStart;

  PlayingSound()
  {
    memset(this, 0, sizeof(*this));
  }

  bool isEmpty()
  {
    return !sound && !stopMode && !waitingStart;
  }

  void setStopMode()
  {
    if (!sound)
    {
      waitingStart = false;
      return;
    }

    if (stopMode)
    {
      waitingStart = false;
      return;
    }

    version += MAX_PLAYING_SOUNDS;

    if (waitingStart)
    {
      waitingStart = false;
      sound = nullptr;
      return;
    }

    if (channels == 1)
    {
      float val = sound->getData()[unsigned(pos)];
      volumeL *= val;
      volumeR *= val;
    }
    else
    {
      volumeL *= sound->getData()[unsigned(pos) * 2];
      volumeR *= sound->getData()[unsigned(pos) * 2 + 1];
    }
    volumeTrendL = sign(volumeL) * -(1.f / 10000);
    volumeTrendR = sign(volumeR) * -(1.f / 10000);
    stopMode = true;
    sound = nullptr;
  }

  void mixTo(float * __restrict mix, int count, int frequency, double inv_frequency, double buffer_time)
  {
    G_UNUSED(frequency);
    float wishVolumeL = master_volume * volume * min(1.0f + pan, 1.0f);
    float wishVolumeR = master_volume * volume * min(1.0f - pan, 1.0f);
    float * __restrict sndData = sound ? sound->getData() : nullptr;

    double advance = sound ? double(sound->frequency) * inv_frequency * pitch : 1.0;

    if (!stopMode && !waitingStart && sound && volumeL > 0.0f && volumeR > 0.0f &&
        wishVolumeL == volumeL && wishVolumeR == volumeR &&
        pos + advance * count < stopPos &&
        sndData != nullptr
       )
    {
      if (channels == 1)
      {
        for (int i = 0; i < count; i++, mix += 2)
        {
          unsigned ip = unsigned(pos);
          float t = float(pos - ip);
          float v = lerp(sndData[ip], sndData[ip + 1], t);
          mix[0] += v * volumeL;
          mix[1] += v * volumeR;
          pos += advance;
        }
      }
      else // channels == 2
      {
        for (int i = 0; i < count; i++, mix += 2)
        {
          unsigned ip = unsigned(pos);
          float t = float(pos - ip);
          float vl = lerp(sndData[ip * 2], sndData[ip * 2 + 2], t);
          float vr = lerp(sndData[ip * 2 + 1], sndData[ip * 2 + 2 + 1], t);
          mix[0] += vl * volumeL;
          mix[1] += vr * volumeR;
          pos += advance;
        }
      }

      return;
    }

    if (waitingStart && timeToStart > buffer_time)
    {
      timeToStart -= buffer_time;
      return;
    }

    if (!sndData && !stopMode)
      stopMode = true;

    if (channels == 1)
    {
      for (int i = 0; i < count; i++, mix += 2)
      {
        if (waitingStart)
        {
          timeToStart -= inv_frequency;
          if (timeToStart <= 0.0)
          {
            waitingStart = false;
            pos = startPos;
          }
        }
        else if (!stopMode)
        {
          unsigned ip = unsigned(pos);
          float t = float(pos - ip);
          float v = lerp(sndData[ip], sndData[ip + 1], t);

          mix[0] += v * volumeL;
          mix[1] += v * volumeR;

          if (volumeL != wishVolumeL)
          {
            if (fabsf(volumeL - wishVolumeL) <= ONE_DIV_512)
              volumeL = wishVolumeL;
            else if (volumeL < wishVolumeL)
              volumeL += ONE_DIV_512;
            else
              volumeL -= ONE_DIV_512;
          }

          if (volumeR != wishVolumeR)
          {
            if (fabsf(volumeR - wishVolumeR) <= ONE_DIV_512)
              volumeR = wishVolumeR;
            else if (volumeR < wishVolumeR)
              volumeR += ONE_DIV_512;
            else
              volumeR -= ONE_DIV_512;
          }

          pos += advance;
          if (pos >= stopPos)
          {
            if (loop)
              pos = startPos;
            else
            {
              pos = stopPos;
              setStopMode();
            }
          }
        }
        else // stopMode
        {
          if (fabsf(volumeL) <= ONE_DIV_512)
            volumeL = 0.0f;
          else
          {
            volumeL += volumeTrendL;
            volumeL *= 0.997f;
          }

          if (fabsf(volumeR) <= ONE_DIV_512)
            volumeR = 0.0f;
          else
          {
            volumeR += volumeTrendR;
            volumeR *= 0.997f;
          }

          if (volumeR == 0.f && volumeL == 0.f)
          {
            stopMode = false;
            break;
          }

          mix[0] += volumeL;
          mix[1] += volumeR;
        }
      }
    }
    else // channels == 2
    {
      for (int i = 0; i < count; i++, mix += 2)
      {
        if (waitingStart)
        {
          timeToStart -= inv_frequency;
          if (timeToStart <= 0.0)
          {
            waitingStart = false;
            pos = startPos;
          }
        }
        else if (!stopMode)
        {
          unsigned ip = unsigned(pos);
          float t = float(pos - ip);
          float vl = lerp(sndData[ip * 2], sndData[ip * 2 + 2], t);
          float vr = lerp(sndData[ip * 2 + 1], sndData[ip * 2 + 2 + 1], t);

          mix[0] += vl * volumeL;
          mix[1] += vr * volumeR;

          if (volumeL != wishVolumeL)
          {
            if (fabsf(volumeL - wishVolumeL) <= ONE_DIV_512)
              volumeL = wishVolumeL;
            else if (volumeL < wishVolumeL)
              volumeL += ONE_DIV_512;
            else
              volumeL -= ONE_DIV_512;
          }

          if (volumeR != wishVolumeR)
          {
            if (fabsf(volumeR - wishVolumeR) <= ONE_DIV_512)
              volumeR = wishVolumeR;
            else if (volumeR < wishVolumeR)
              volumeR += ONE_DIV_512;
            else
              volumeR -= ONE_DIV_512;
          }

          pos += advance;
          if (pos >= stopPos)
          {
            if (loop)
              pos = startPos;
            else
            {
              pos = stopPos;
              setStopMode();
            }
          }
        }
        else // stopMode
        {
          if (fabsf(volumeL) <= ONE_DIV_512)
            volumeL = 0.0f;
          else
          {
            volumeL += volumeTrendL;
            volumeL *= 0.997f;
          }

          if (fabsf(volumeR) <= ONE_DIV_512)
            volumeR = 0.0f;
          else
          {
            volumeR += volumeTrendR;
            volumeR *= 0.997f;
          }

          if (volumeR == 0.f && volumeL == 0.f)
          {
            stopMode = false;
            break;
          }

          mix[0] += volumeL;
          mix[1] += volumeR;
        }
      }
    }
  }
};

static array<PlayingSound, MAX_PLAYING_SOUNDS> playing_sounds;

static int allocate_playing_sound()
{
  for (int i = 1; i < MAX_PLAYING_SOUNDS; i++)
    if (playing_sounds[i].isEmpty())
    {
      playing_sounds[i].version += MAX_PLAYING_SOUNDS;
      return i;
    }
  return -1;
}

static bool is_handle_valid(PlayingSoundHandle ps)
{
  unsigned idx = ps.handle & PLAYING_SOUNDS_MASK;
  return (playing_sounds[idx].version == int(ps.handle & (~PLAYING_SOUNDS_MASK))) && idx > 0;
}

static int handle_to_index(PlayingSoundHandle ps)
{
  if (!is_handle_valid(ps))
    return -1;
  return ps.handle & PLAYING_SOUNDS_MASK;
}


static float g_limiter_mult = 1.0f;

static void apply_limiter(float * __restrict buf, int count)
{
  float limiter_mult = g_limiter_mult;
  for (int i = 0; i < count; i++, buf++)
  {
    float v = *buf * limiter_mult;
    *buf = v;
    if (fabsf(v) > 1.0f)
      limiter_mult *= 0.96f;
    if (limiter_mult < 1.0f)
      limiter_mult = min(limiter_mult + (0.5f / 65536), 1.0f);
  }

  g_limiter_mult = limiter_mult;
}


static void fill_buffer_cb(float * __restrict out_buf, int frequency, int channels, int samples)
{
  int cnt = 0;
  double total_time_played_ = total_time_played;
  {
    lock_guard<mutex> lock(sound_cs);
    float * mixCursor = out_buf;
    int samplesLeft = samples;
    memset(out_buf, 0, samples * channels * sizeof(float));

    double invFrequency = 1.0 / frequency;
    int step = 256;

    while (samplesLeft > 0)
    {
      cnt = 0;
      for (auto && s : playing_sounds)
        if (!s.isEmpty())
        {
          cnt++;
          s.mixTo(mixCursor, min(samplesLeft, step), frequency, invFrequency, min(samplesLeft, step) * invFrequency);
        }

      samplesLeft -= step;
      mixCursor += step * channels;
      total_samples_played += min(samplesLeft, step);
      total_time_played_ += min(samplesLeft, step) * invFrequency;
    }
  }
  apply_limiter(out_buf, samples * channels);
  playing_sound_count = cnt;

  total_time_played = total_time_played_;
}



static ma_device miniaudio_device;
static volatile bool device_initialized = false;
static ma_log ma_log_struct = { 0 };
static ma_context audio_context = { 0 };


void on_error_log(void * user_data, ma_uint32 level, const char * message)
{
  G_UNUSED(user_data);
  G_UNUSED(level);
  if (level <= 1)
    LOG(LogLevel::error) << message;
  else
    LOG(LogLevel::info) << message;
}

static void miniaudio_data_callback(ma_device* p_device, void* p_output, const void* p_input, ma_uint32 frame_count)
{
  G_UNUSED(p_input);

  if (!device_initialized)
  {
    memset(p_output, 0, frame_count * ma_get_bytes_per_frame(p_device->playback.format, p_device->playback.channels));
    return;
  }

  fill_buffer_cb((float *)p_output, OUTPUT_SAMPLE_RATE, p_device->playback.channels, frame_count);
}

void init_sound_lib_internal()
{
  if (device_initialized)
    return;

  ma_log_init(nullptr, &ma_log_struct);
  ma_log_register_callback(&ma_log_struct, {on_error_log, nullptr});

  ma_context_init(NULL, 0, NULL, &audio_context);
  audio_context.pLog = &ma_log_struct;

  ma_device_config deviceConfig;

  deviceConfig = ma_device_config_init(ma_device_type_playback);
  deviceConfig.playback.format = ma_format_f32;
  deviceConfig.playback.channels = OUTPUT_CHANNELS;
  deviceConfig.sampleRate = OUTPUT_SAMPLE_RATE;
  deviceConfig.dataCallback = miniaudio_data_callback;
  deviceConfig.pUserData = nullptr;

  if (ma_device_init(&audio_context, &deviceConfig, &miniaudio_device) != MA_SUCCESS)
  {
    LOG(LogLevel::error) << "SOUND: Failed to open playback device";
    return;
  }

  LOG() << "Sound device name: " << miniaudio_device.playback.name;

  if (ma_device_start(&miniaudio_device) != MA_SUCCESS)
  {
    LOG(LogLevel::error) << "SOUND: Failed to start playback device";
    ma_device_uninit(&miniaudio_device);
    return;
  }

  device_initialized = true;
}

void print_debug_infos(int from_frame)
{
  for (auto && dbg : dbg_pointers)
    if (dbg && dbg->creationFrame >= from_frame)
      LOG() << "  sound: " << dbg->name;
}

void initialize()
{
  memset(&playing_sounds[0], 0, sizeof(playing_sounds[0]) * playing_sounds.size());
}

void finalize()
{
  device_initialized = false;
  lock_guard<mutex> lock(sound_cs);
  ma_device_uninit(&miniaudio_device);
}


PcmSound create_sound(int frequency, const TArray<float> & data)
{
  if (!device_initialized)
    init_sound_lib_internal();

  if (frequency < 1 || !data.size)
    return PcmSound();

  PcmSound s;
  s.frequency = frequency;
  s.channels = 1;
  s.samples = data.size;
  s.newData(s.getDataMemorySize());
  memcpy(s.getData(), data.data, s.getDataMemorySize());
  s.getData()[data.size] = s.getData()[0];

  s.dbg = new DasboxDebugInfo();
  snprintf(s.dbg->name, sizeof(s.dbg->name) - 1, "mono %d smpl @%d", s.samples, s.frequency);
  dbg_pointers.insert(s.dbg);

  return s;
}

PcmSound create_sound_stereo(int frequency, const TArray<float2> & data)
{
  if (!device_initialized)
    init_sound_lib_internal();

  if (frequency < 1 || !data.size)
    return PcmSound();

  PcmSound s;
  s.frequency = frequency;
  s.channels = 2;
  s.samples = data.size;
  s.newData(s.getDataMemorySize());
  memcpy(s.getData(), data.data, s.getDataMemorySize());
  s.getData()[s.samples * 2] = s.getData()[0];
  s.getData()[s.samples * 2 + 1] = s.getData()[1];

  s.dbg = new DasboxDebugInfo();
  snprintf(s.dbg->name, sizeof(s.dbg->name) - 1, "stereo %d smpl @%d", s.samples, s.frequency);
  dbg_pointers.insert(s.dbg);

  return s;
}


PcmSound create_sound_from_file(const char * file_name)
{
  if (!device_initialized)
    init_sound_lib_internal();

  if (!file_name || !file_name[0])
  {
    LOG(LogLevel::error) << "Cannot create sound. File name is empty. '" << file_name << "'";
    return PcmSound();
  }

/*
  if (!fs::is_path_string_valid(file_name))
  {
    LOG(LogLevel::error) << "Cannot open sound '" << file_name << "'. Absolute paths or access to the parent directory is prohibited.";
    return PcmSound();
  }
  */

  PcmSound s;

  unsigned int channels;
  unsigned int sampleRate;
  drwav_uint64 totalPCMFrameCount;
  float * pSampleData = nullptr;

  const char * p = strrchr(file_name, '.');
  if (p && !stricmp(p, ".wav") && !p[4])
    pSampleData = drwav_open_file_and_read_pcm_frames_f32(file_name, &channels, &sampleRate, &totalPCMFrameCount, nullptr);
  else if (p && !stricmp(p, ".mp3") && !p[4])
  {
    drmp3_config config = { 0 };
    pSampleData = drmp3_open_file_and_read_pcm_frames_f32(file_name, &config, &totalPCMFrameCount, nullptr);
    channels = config.channels;
    sampleRate = config.sampleRate;
  }
  else if (p && !stricmp(p, ".flac") && !p[4])
    pSampleData = drwav_open_file_and_read_pcm_frames_f32(file_name, &channels, &sampleRate, &totalPCMFrameCount, nullptr);
  else
  {
    LOG(LogLevel::error) << "Cannot create sound from '" << file_name << "', unrecognized file format. Expected .wav, .flac or .mp3";
    return PcmSound();
  }

  if (!pSampleData)
  {
    LOG(LogLevel::error) << "Cannot create sound from '" << file_name << "'";
    return PcmSound();
  }

  if (channels != 1 && channels != 2)
  {
    LOG(LogLevel::error) << "Cannot create sound from '" << file_name << "', invalid channels count =" << int(channels);
    return PcmSound();
  }

  s.channels = int(channels);
  s.frequency = int(sampleRate);
  s.samples = int(totalPCMFrameCount);
  s.newData(s.getDataMemorySize());
  memcpy(s.getData(), pSampleData, channels * s.samples * sizeof(float));
  if (s.channels == 2)
  {
    s.getData()[s.samples * 2] = s.getData()[0];
    s.getData()[s.samples * 2 + 1] = s.getData()[1];
  }
  else
  {
    s.getData()[s.samples] = s.getData()[0];
  }

  drwav_free(pSampleData, NULL);

  s.dbg = new DasboxDebugInfo();
  snprintf(s.dbg->name, sizeof(s.dbg->name) - 1, "%s", file_name);
  dbg_pointers.insert(s.dbg);

  return s;
}


void get_sound_data(const PcmSound & sound, TArray<float> & out_data)
{
  if (!sound.getData())
    return;

  int count = sound.samples;
  if (count > int(out_data.size))
    count = int(out_data.size);

  if (count)
  {
    if (sound.channels == 1)
      memcpy(out_data.data, sound.getData(), count * sizeof(float));
    else if (sound.channels == 2)
    {
      float * __restrict ptr = (float *)out_data.data;
      float * __restrict soundData = (float *)sound.getData();
      for (int i = 0; i < count; i++)
        ptr[i] = (soundData[i * 2] + soundData[i * 2 + 1]) * 0.5f;
    }
  }
}

void get_sound_data_stereo(const PcmSound & sound, TArray<float2> & out_data)
{
  if (!sound.getData())
    return;

  int count = sound.samples;
  if (count > int(out_data.size))
    count = int(out_data.size);

  if (count)
  {
    if (sound.channels == 2)
      memcpy(out_data.data, sound.getData(), count * sizeof(float) * 2);
    else if (sound.channels == 1)
    {
      float * __restrict ptr = (float *)out_data.data;
      float * __restrict soundData = (float *)sound.getData();
      for (int i = 0; i < count; i++)
      {
        ptr[i * 2] = soundData[i];
        ptr[i * 2 + 1] = soundData[i];
      }
    }
  }
}

void set_sound_data(PcmSound & sound, const TArray<float> & in_data)
{
  if (!sound.getData())
    return;

  int count = sound.samples;
  if (count > int(in_data.size))
    count = int(in_data.size);

  if (!count)
    return;

  if (sound.channels == 1)
  {
    memcpy(sound.getData(), in_data.data, count * sizeof(float));
    sound.getData()[sound.samples] = sound.getData()[0];
  }
  else if (sound.channels == 2)
  {
    float * __restrict ptr = (float *)in_data.data;
    float * __restrict soundData = (float *)sound.getData();
    for (int i = 0; i < count; i++)
    {
      soundData[i * 2] = ptr[i];
      soundData[i * 2 + 1] = ptr[i];
    }
    soundData[sound.samples * 2] = soundData[0];
    soundData[sound.samples * 2 + 1] = soundData[1];
  }
}

void set_sound_data_stereo(PcmSound & sound, const TArray<float2> & in_data)
{
  if (!sound.getData())
    return;

  int count = sound.samples;
  if (count > int(in_data.size))
    count = int(in_data.size);

  if (!count)
    return;

  if (sound.channels == 1)
  {
    float * __restrict ptr = (float *)in_data.data;
    float * __restrict soundData = (float *)sound.getData();
    for (int i = 0; i < count; i++)
      soundData[i] = (ptr[i * 2] + ptr[i * 2 + 1]) * 0.5f;

    sound.getData()[sound.samples] = sound.getData()[0];
  }
  else if (sound.channels == 2)
  {
    memcpy(sound.getData(), in_data.data, 2 * count * sizeof(float));
    sound.getData()[sound.samples * 2] = sound.getData()[0];
    sound.getData()[sound.samples * 2 + 1] = sound.getData()[1];
  }
}

void delete_sound(PcmSound * sound)
{
  lock_guard<mutex> lock(sound_cs);

  for (auto && s : playing_sounds)
    if (s.sound == sound)
      if (!s.isEmpty())
        s.setStopMode();

  sound->deleteData();
  sound->data = nullptr;
  sound->samples = 0;
}

void delete_allocated_sounds()
{
  lock_guard<mutex> lock(sound_cs);
  for (auto && data : sound_data_pointers)
    delete[] data;

  sound_data_pointers.clear();

  for (auto && dbg : dbg_pointers)
    delete dbg;

  dbg_pointers.clear();

  memory_used = 0;
}


PlayingSoundHandle play_sound_internal(const PcmSound & sound, float volume, float pitch, float pan, float start_time, float end_time,
                                       bool loop, float defer_time_sec)
{
  if (!device_initialized)
    init_sound_lib_internal();

  lock_guard<mutex> lock(sound_cs);

  int idx = allocate_playing_sound();
  if (idx < 0 || sound.samples <= 2)
    return PlayingSoundHandle();

  PlayingSound & s = playing_sounds[idx];

  pitch = clamp(pitch, 0.00001f, 1000.0f);
  pan = clamp(pan, -1.0f, 1.0f);
  volume = clamp(volume, 0.0f, 100000.0f);

  double start = clamp(double(int64_t(start_time * sound.frequency)), 0.0, double(sound.samples - 1));
  double stop = clamp(double(int64_t(end_time * sound.frequency)), start, double(sound.samples - 1));
  double pos = start;
  if (defer_time_sec < 0.0f)
    pos = min(double(int(-defer_time_sec * sound.frequency)), stop);

  s.channels = sound.channels;
  s.sound = &sound;
  s.volume = volume;
  s.pitch = pitch;
  s.pan = pan;
  s.volumeL = master_volume * volume * min(1.0f + pan, 1.0f);
  s.volumeR = master_volume * volume * min(1.0f - pan, 1.0f);

  s.pos = pos;
  s.startPos = start;
  s.stopPos = stop;
  s.loop = loop;
  s.stopMode = false;
  s.timeToStart = max(defer_time_sec, 0.0f);
  s.waitingStart = (s.timeToStart != 0.0);

  PlayingSoundHandle res;
  res.handle = idx | s.version;

  return res;
}


PlayingSoundHandle play_sound_1(const PcmSound & sound)
{
  return play_sound_internal(sound, 1.0f, 1.0f, 0.0f, 0.0f, VERY_BIG_NUMBER, false, 0.0f);
}

PlayingSoundHandle play_sound_2(const PcmSound & sound, float volume)
{
  return play_sound_internal(sound, volume, 1.0f, 0.0f, 0.0f, VERY_BIG_NUMBER, false, 0.0f);
}

PlayingSoundHandle play_sound_3(const PcmSound & sound, float volume, float pitch)
{
  return play_sound_internal(sound, volume, pitch, 0.0f, 0.0f, VERY_BIG_NUMBER, false, 0.0f);
}

PlayingSoundHandle play_sound_4(const PcmSound & sound, float volume, float pitch, float pan)
{
  return play_sound_internal(sound, volume, pitch, pan, 0.0f, VERY_BIG_NUMBER, false, 0.0f);
}

PlayingSoundHandle play_sound_5(const PcmSound & sound, float volume, float pitch, float pan, float start_time, float end_time)
{
  return play_sound_internal(sound, volume, pitch, pan, start_time, end_time, false, 0.0f);
}

PlayingSoundHandle play_sound_loop_1(const PcmSound & sound)
{
  return play_sound_internal(sound, 1.0f, 1.0f, 0.0f, 0.0f, VERY_BIG_NUMBER, true, 0.0f);
}

PlayingSoundHandle play_sound_loop_2(const PcmSound & sound, float volume)
{
  return play_sound_internal(sound, volume, 1.0f, 0.0f, 0.0f, VERY_BIG_NUMBER, true, 0.0f);
}

PlayingSoundHandle play_sound_loop_3(const PcmSound & sound, float volume, float pitch)
{
  return play_sound_internal(sound, volume, pitch, 0.0f, 0.0f, VERY_BIG_NUMBER, true, 0.0f);
}

PlayingSoundHandle play_sound_loop_4(const PcmSound & sound, float volume, float pitch, float pan)
{
  return play_sound_internal(sound, volume, pitch, pan, 0.0f, VERY_BIG_NUMBER, true, 0.0f);
}

PlayingSoundHandle play_sound_loop_5(const PcmSound & sound, float volume, float pitch, float pan, float start_time, float end_time)
{
  return play_sound_internal(sound, volume, pitch, pan, start_time, end_time, true, 0.0f);
}


PlayingSoundHandle play_sound_deferred_1(const PcmSound & sound, float defer_seconds)
{
  return play_sound_internal(sound, 1.0f, 1.0f, 0.0f, 0.0f, VERY_BIG_NUMBER, false, defer_seconds);
}

PlayingSoundHandle play_sound_deferred_2(const PcmSound & sound, float defer_seconds, float volume)
{
  return play_sound_internal(sound, volume, 1.0f, 0.0f, 0.0f, VERY_BIG_NUMBER, false, defer_seconds);
}

PlayingSoundHandle play_sound_deferred_3(const PcmSound & sound, float defer_seconds, float volume, float pitch)
{
  return play_sound_internal(sound, volume, pitch, 0.0f, 0.0f, VERY_BIG_NUMBER, false, defer_seconds);
}

PlayingSoundHandle play_sound_deferred_4(const PcmSound & sound, float defer_seconds, float volume, float pitch, float pan)
{
  return play_sound_internal(sound, volume, pitch, pan, 0.0f, VERY_BIG_NUMBER, false, defer_seconds);
}

PlayingSoundHandle play_sound_deferred_5(const PcmSound & sound, float defer_seconds, float volume, float pitch, float pan,
  float start_time, float end_time)
{
  return play_sound_internal(sound, volume, pitch, pan, start_time, end_time, false, defer_seconds);
}


void set_sound_pitch(PlayingSoundHandle handle, float pitch)
{
  lock_guard<mutex> lock(sound_cs);

  int idx = handle_to_index(handle);
  if (idx < 0)
    return;
  playing_sounds[idx].pitch = pitch;
}

void set_sound_volume(PlayingSoundHandle handle, float volume)
{
  lock_guard<mutex> lock(sound_cs);

  int idx = handle_to_index(handle);
  if (idx < 0)
    return;
  playing_sounds[idx].volume = volume;
}

void set_sound_pan(PlayingSoundHandle handle, float pan)
{
  lock_guard<mutex> lock(sound_cs);

  int idx = handle_to_index(handle);
  if (idx < 0)
    return;
  playing_sounds[idx].pan = pan;
}

bool is_playing(PlayingSoundHandle handle)
{
  int idx = handle_to_index(handle);
  if (idx < 0 || playing_sounds[idx].stopMode)
    return false;

  return true;
}

float get_sound_play_pos(PlayingSoundHandle handle)
{
  lock_guard<mutex> lock(sound_cs);

  int idx = handle_to_index(handle);
  if (idx < 0)
    return 0.0f;

  if (!playing_sounds[idx].sound || playing_sounds[idx].stopMode || playing_sounds[idx].waitingStart)
    return 0.0f;

  return float(playing_sounds[idx].pos / playing_sounds[idx].sound->frequency);
}

void set_sound_play_pos(PlayingSoundHandle handle, float pos_seconds)
{
  lock_guard<mutex> lock(sound_cs);

  int idx = handle_to_index(handle);
  if (idx < 0)
    return;

  if (!playing_sounds[idx].sound || playing_sounds[idx].stopMode)
    return;

  double p = floor(playing_sounds[idx].sound->frequency * pos_seconds);
  playing_sounds[idx].pos = clamp(p, playing_sounds[idx].startPos, playing_sounds[idx].stopPos);
}

void stop_sound(PlayingSoundHandle handle)
{
  lock_guard<mutex> lock(sound_cs);

  int idx = handle_to_index(handle);
  if (idx < 0)
    return;

  if (!playing_sounds[idx].sound || playing_sounds[idx].stopMode)
    return;

  playing_sounds[idx].setStopMode();
}

void stop_all_sounds()
{
  lock_guard<mutex> lock(sound_cs);

  for (auto && s : playing_sounds)
    if (!s.isEmpty())
      s.setStopMode();
}

void enter_sound_critical_section()
{
  if (sound_cs_manual_entered)
    return;

  sound_cs.lock();
  sound_cs_manual_entered = true;
}

void leave_sound_critical_section()
{
  if (sound_cs_manual_entered)
  {
    sound_cs_manual_entered = false;
    sound_cs.unlock();
  }
}

void set_master_volume(float volume)
{
  lock_guard<mutex> lock(sound_cs);
  master_volume = volume;
}

float get_output_sample_rate()
{
  return OUTPUT_SAMPLE_RATE;
}

int64_t get_total_samples_played()
{
  return total_samples_played;
}

double get_total_time_played()
{
  return total_time_played;
}

double get_memory_used()
{
  return double(memory_used);
}


PcmSound::PcmSound(PcmSound && b)
{
  lock_guard<mutex> lock(sound_cs);

  for (auto && s : playing_sounds)
    if (s.sound == this || s.sound == &b)
      if (!s.isEmpty())
        s.setStopMode();

  frequency = b.frequency;
  samples = b.samples;
  channels = b.channels;
  memoryUsed = b.memoryUsed;
  data = b.data;
  dbg = b.dbg;

  b.memoryUsed = 0;
  b.data = nullptr;
  b.dbg = nullptr;
}

PcmSound& PcmSound::operator=(const PcmSound & b)
{
  if (this == &b)
    return *this;

  lock_guard<mutex> lock(sound_cs);

  for (auto && s : playing_sounds)
    if (s.sound == this)
      if (!s.isEmpty())
        s.setStopMode();

  deleteData();
  frequency = b.frequency;
  samples = b.samples;
  channels = b.channels;
  newData(getDataMemorySize());
  memcpy(data, b.data, getDataMemorySize());
  dbg = b.dbg ? new DasboxDebugInfo(*b.dbg) : nullptr;
  if (dbg)
    dbg->creationFrame = current_frame;
  return *this;
}

PcmSound& PcmSound::operator=(PcmSound && b)
{
  if (this == &b)
    return *this;

  lock_guard<mutex> lock(sound_cs);

  for (auto && s : playing_sounds)
    if (s.sound == this || s.sound == &b)
      if (!s.isEmpty())
        s.setStopMode();

  frequency = b.frequency;
  samples = b.samples;
  channels = b.channels;
  data = b.data;
  dbg = b.dbg;
  memoryUsed = b.memoryUsed;

  b.data = nullptr;
  b.dbg = nullptr;
  b.memoryUsed = 0;

  return *this;
}


PcmSound::~PcmSound()
{
  if (!data)
    return;

  lock_guard<mutex> lock(sound_cs);

  for (auto && s : playing_sounds)
    if (s.sound == this)
      if (!s.isEmpty())
        s.setStopMode();

  deleteData();
  samples = 0;
}




struct SimNode_DeletePcmSound : SimNode_Delete
{
  SimNode_DeletePcmSound( const LineInfo & a, SimNode * s, uint32_t t )
    : SimNode_Delete(a, s, t) {}

  virtual SimNode * visit(SimVisitor & vis) override
  {
    V_BEGIN();
    V_OP(DeletePcmSound);
    V_ARG(total);
    V_SUB(subexpr);
    V_END();
  }

  virtual vec4f DAS_EVAL_ABI eval(Context & context) override
  {
    DAS_PROFILE_NODE
      auto pH = (sound::PcmSound *)subexpr->evalPtr(context);
    for (uint32_t i = 0; i != total; ++i, pH++)
      sound::delete_sound(pH);
    return v_zero();
  }
};


struct PcmSoundAnnotation : ManagedStructureAnnotation<sound::PcmSound, true, true>
{
  PcmSoundAnnotation(ModuleLibrary & ml) : ManagedStructureAnnotation("PcmSound", ml, "das::sound::PcmSound")
  {
    addProperty<DAS_BIND_MANAGED_PROP(getDuration)>("duration");
    addProperty<DAS_BIND_MANAGED_PROP(getFrequency)>("frequency");
    addProperty<DAS_BIND_MANAGED_PROP(getSamples)>("samples");
    addProperty<DAS_BIND_MANAGED_PROP(getChannels)>("channels");
    addProperty<DAS_BIND_MANAGED_PROP(isValid)>("valid");
  }

  bool canCopy() const override { return false; }
  virtual bool hasNonTrivialCtor() const override { return false; }
  virtual bool isLocal() const override { return true; }
  virtual bool canClone() const override { return true; }
  virtual bool canMove() const override { return true; }
  virtual bool canNew() const override { return true; }
  virtual bool canDelete() const override { return true; }
  virtual bool needDelete() const override { return true; }
  virtual bool canBePlacedInContainer() const override { return true; }

  virtual SimNode * simulateDelete(Context & context, const LineInfo & at, SimNode * sube, uint32_t count) const override
  {
    return context.code->makeNode<SimNode_DeletePcmSound>(at, sube, count);
  }
};



struct PlayingSoundHandleAnnotation final: ManagedValueAnnotation<sound::PlayingSoundHandle>
{
  PlayingSoundHandleAnnotation(ModuleLibrary & mlib) : ManagedValueAnnotation(mlib, "PlayingSoundHandle", "das::sound::PlayingSoundHandle")
  {
  }

  virtual void walk(DataWalker & walker, void * data) override
  {
    walker.UInt(((sound::PlayingSoundHandle *)data)->handle);
  }

  virtual bool canCopy() const override { return true; }
  virtual bool hasNonTrivialCtor() const override { return false; }
  virtual bool isLocal() const override { return true; }
  virtual bool canClone() const override { return true; }
  virtual bool canMove() const override { return true; }
  virtual bool canNew() const override { return true; }
  virtual bool canBePlacedInContainer() const override { return true; }
};

struct Opl3ChipAnnotation : ManagedStructureAnnotation<opl3_chip> {
    Opl3ChipAnnotation ( ModuleLibrary & mlib ) : ManagedStructureAnnotation("Opl3Chip", mlib, "opl3_chip") {
    }
};

class Module_Sound : public das::Module {
public:
    Module_Sound() : Module("sound") {
        ModuleLibrary lib;
        lib.addModule(this);
        lib.addBuiltInModule();

        // opl3
        addAnnotation(make_smart<Opl3ChipAnnotation>(lib));
        addExtern<DAS_BIND_FUN(OPL3_Generate)>(*this, lib, "OPL3_Generate",
            SideEffects::worstDefault, "OPL3_Generate")->args({"chip", "buf"});
        addExtern<DAS_BIND_FUN(OPL3_GenerateResampled)>(*this, lib, "OPL3_GenerateResampled",
            SideEffects::worstDefault, "OPL3_GenerateResampled")->args({"chip", "buf"});
        addExtern<DAS_BIND_FUN(OPL3_Reset)>(*this, lib, "OPL3_Reset",
            SideEffects::worstDefault, "OPL3_Reset")->args({"chip", "sampleRate"});
        addExtern<DAS_BIND_FUN(OPL3_WriteReg)>(*this, lib, "OPL3_WriteReg",
            SideEffects::worstDefault, "OPL3_WriteReg")->args({"chip", "reg", "v"});
        addExtern<DAS_BIND_FUN(OPL3_WriteRegBuffered)>(*this, lib, "OPL3_WriteRegBuffered",
            SideEffects::worstDefault, "OPL3_WriteRegBuffered")->args({"chip", "reg", "v"});
        addExtern<DAS_BIND_FUN(OPL3_GenerateStream)>(*this, lib, "OPL3_GenerateStream",
            SideEffects::worstDefault, "OPL3_GenerateStream")->args({"OPL3_GenerateStream", "sndptr", "numsamples"});
        addExtern<DAS_BIND_FUN(OPL3_Generate4Ch)>(*this, lib, "OPL3_Generate4Ch",
            SideEffects::worstDefault, "OPL3_Generate4Ch")->args({"chip", "buf"});
        addExtern<DAS_BIND_FUN(OPL3_Generate4ChResampled)>(*this, lib, "OPL3_Generate4ChResampled",
            SideEffects::worstDefault, "OPL3_Generate4ChResampled")->args({"chip", "buf"});
        addExtern<DAS_BIND_FUN(OPL3_Generate4ChStream)>(*this, lib, "OPL3_Generate4ChStream",
            SideEffects::worstDefault, "OPL3_Generate4ChStream")->args({"chip", "sndptr1", "sndptr2", "numsamples"});

        addAnnotation(das::make_smart<PlayingSoundHandleAnnotation>(lib));
        addAnnotation(das::make_smart<PcmSoundAnnotation>(lib));
        addCtorAndUsing<sound::PcmSound>(*this, lib, "PcmSound", "sound::PcmSound");

        addExtern<DAS_BIND_FUN(sound::initialize)>(*this, lib,
          "sound_initialize", SideEffects::modifyExternal, "sound::initialize");

        addExtern<DAS_BIND_FUN(sound::finalize)>(*this, lib,
          "sound_finalize", SideEffects::modifyExternal, "sound::finalize");


        addExtern<DAS_BIND_FUN(sound::create_sound), SimNode_ExtFuncCallAndCopyOrMove>(*this, lib,
          "create_sound", SideEffects::modifyExternal, "sound::create_sound")
          ->args({"frequency", "data"});

        addExtern<DAS_BIND_FUN(sound::create_sound_stereo), SimNode_ExtFuncCallAndCopyOrMove>(*this, lib,
          "create_sound", SideEffects::modifyExternal, "sound::create_sound_stereo")
          ->args({"frequency", "data"});

        addExtern<DAS_BIND_FUN(sound::create_sound_from_file), SimNode_ExtFuncCallAndCopyOrMove>(*this, lib,
          "create_sound", SideEffects::modifyExternal, "sound::create_sound_from_file")
          ->args({"file_name"});

        addExtern<DAS_BIND_FUN(sound::get_sound_data)>(*this, lib,
          "get_sound_data", SideEffects::modifyArgumentAndExternal, "sound::get_sound_data")
          ->args({"sound", "out_data"});

        addExtern<DAS_BIND_FUN(sound::get_sound_data_stereo)>(*this, lib,
          "get_sound_data", SideEffects::modifyArgumentAndExternal, "sound::get_sound_data_stereo")
          ->args({"sound", "out_data"});

        addExtern<DAS_BIND_FUN(sound::set_sound_data)>(*this, lib,
          "set_sound_data", SideEffects::modifyExternal, "sound::set_sound_data")
          ->args({"sound", "in_data"});

        addExtern<DAS_BIND_FUN(sound::set_sound_data_stereo)>(*this, lib,
          "set_sound_data", SideEffects::modifyExternal, "sound::set_sound_data_stereo")
          ->args({"sound", "in_data"});

        addExtern<DAS_BIND_FUN(sound::play_sound_1)>(*this, lib,
          "play_sound", SideEffects::modifyExternal, "sound::play_sound_1")
          ->args({"sound"});

        addExtern<DAS_BIND_FUN(sound::play_sound_2)>(*this, lib,
          "play_sound", SideEffects::modifyExternal, "sound::play_sound_2")
          ->args({"sound", "volume"});

        addExtern<DAS_BIND_FUN(sound::play_sound_3)>(*this, lib,
          "play_sound", SideEffects::modifyExternal, "sound::play_sound_3")
          ->args({"sound", "volume", "pitch"});

        addExtern<DAS_BIND_FUN(sound::play_sound_4)>(*this, lib,
          "play_sound", SideEffects::modifyExternal, "sound::play_sound_4")
          ->args({"sound", "volume", "pitch", "pan"});

        addExtern<DAS_BIND_FUN(sound::play_sound_5)>(*this, lib,
          "play_sound", SideEffects::modifyExternal, "sound::play_sound_5")
          ->args({"sound", "volume", "pitch", "pan", "start_time", "stop_time"});

        addExtern<DAS_BIND_FUN(sound::play_sound_loop_1)>(*this, lib,
          "play_sound_loop", SideEffects::modifyExternal, "sound::play_sound_loop_1")
          ->args({"sound"});

        addExtern<DAS_BIND_FUN(sound::play_sound_loop_2)>(*this, lib,
          "play_sound_loop", SideEffects::modifyExternal, "sound::play_sound_loop_2")
          ->args({"sound", "volume"});

        addExtern<DAS_BIND_FUN(sound::play_sound_loop_3)>(*this, lib,
          "play_sound_loop", SideEffects::modifyExternal, "sound::play_sound_loop_3")
          ->args({"sound", "volume", "pitch"});

        addExtern<DAS_BIND_FUN(sound::play_sound_loop_4)>(*this, lib,
          "play_sound_loop", SideEffects::modifyExternal, "sound::play_sound_loop_4")
          ->args({"sound", "volume", "pitch", "pan"});

        addExtern<DAS_BIND_FUN(sound::play_sound_loop_5)>(*this, lib,
          "play_sound_loop", SideEffects::modifyExternal, "sound::play_sound_loop_5")
          ->args({"sound", "volume", "pitch", "pan", "start_time", "end_time"});

        addExtern<DAS_BIND_FUN(sound::play_sound_deferred_1)>(*this, lib,
          "play_sound_deferred", SideEffects::modifyExternal, "sound::play_sound_deferred_1")
          ->args({"sound", "defer_seconds"});

        addExtern<DAS_BIND_FUN(sound::play_sound_deferred_2)>(*this, lib,
          "play_sound_deferred", SideEffects::modifyExternal, "sound::play_sound_deferred_2")
          ->args({"sound", "defer_seconds", "volume"});

        addExtern<DAS_BIND_FUN(sound::play_sound_deferred_3)>(*this, lib,
          "play_sound_deferred", SideEffects::modifyExternal, "sound::play_sound_deferred_3")
          ->args({"sound", "defer_seconds", "volume", "pitch"});

        addExtern<DAS_BIND_FUN(sound::play_sound_deferred_4)>(*this, lib,
          "play_sound_deferred", SideEffects::modifyExternal, "sound::play_sound_deferred_4")
          ->args({"sound", "defer_seconds", "volume", "pitch", "pan"});

        addExtern<DAS_BIND_FUN(sound::play_sound_deferred_5)>(*this, lib,
          "play_sound_deferred", SideEffects::modifyExternal, "sound::play_sound_deferred_5")
          ->args({"sound", "defer_seconds", "volume", "pitch", "pan", "start_time", "stop_time"});


        addExtern<DAS_BIND_FUN(sound::set_sound_pitch)>(*this, lib,
          "set_sound_pitch", SideEffects::modifyExternal, "sound::set_sound_pitch")
          ->args({"sound_handle", "pitch"});

        addExtern<DAS_BIND_FUN(sound::set_sound_volume)>(*this, lib,
          "set_sound_volume", SideEffects::modifyExternal, "sound::set_sound_volume")
          ->args({"sound_handle", "volume"});

        addExtern<DAS_BIND_FUN(sound::set_sound_pan)>(*this, lib,
          "set_sound_pan", SideEffects::modifyExternal, "sound::set_sound_pan")
          ->args({"sound_handle", "pan"});

        addExtern<DAS_BIND_FUN(sound::is_playing)>(*this, lib,
          "is_playing", SideEffects::accessExternal, "sound::is_playing")
          ->args({"sound_handle"});

        addExtern<DAS_BIND_FUN(sound::get_sound_play_pos)>(*this, lib,
          "get_sound_play_pos", SideEffects::accessExternal, "sound::get_sound_play_pos")
          ->args({"sound_handle"});

        addExtern<DAS_BIND_FUN(sound::set_sound_play_pos)>(*this, lib,
          "set_sound_play_pos", SideEffects::modifyExternal, "sound::set_sound_play_pos")
          ->args({"sound_handle", "pos_seconds"});

        addExtern<DAS_BIND_FUN(sound::stop_sound)>(*this, lib,
          "stop_sound", SideEffects::modifyExternal, "sound::stop_sound")
          ->args({"sound_handle"});


        addExtern<DAS_BIND_FUN(sound::stop_all_sounds)>(*this, lib,
          "stop_all_sounds", SideEffects::modifyExternal, "sound::stop_all_sounds");

        addExtern<DAS_BIND_FUN(sound::enter_sound_critical_section)>(*this, lib,
          "enter_sound_critical_section", SideEffects::modifyExternal, "sound::enter_sound_critical_section");

        addExtern<DAS_BIND_FUN(sound::leave_sound_critical_section)>(*this, lib,
          "leave_sound_critical_section", SideEffects::modifyExternal, "sound::leave_sound_critical_section");

        addExtern<DAS_BIND_FUN(sound::set_master_volume)>(*this, lib,
          "set_master_volume", SideEffects::modifyExternal, "sound::set_master_volume")
          ->args({"volume"});

        addExtern<DAS_BIND_FUN(sound::get_output_sample_rate)>(*this, lib,
          "get_output_sample_rate", SideEffects::accessExternal, "sound::get_output_sample_rate");

        addExtern<DAS_BIND_FUN(sound::get_total_samples_played)>(*this, lib,
          "get_total_samples_played", SideEffects::accessExternal, "sound::get_total_samples_played");

        addExtern<DAS_BIND_FUN(sound::get_total_time_played)>(*this, lib,
          "get_total_time_played", SideEffects::accessExternal, "sound::get_total_time_played");



    }
    virtual ModuleAotType aotRequire ( TextWriter & tw ) const override {
        tw << "#include \"../modules/dasSound/src/dasSound.h\"\n";
        return ModuleAotType::cpp;
    }
};

} //namespace sound

} // namespace das


REGISTER_MODULE_IN_NAMESPACE(Module_Sound, das::sound);
