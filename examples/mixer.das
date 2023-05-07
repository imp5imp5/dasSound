require sound
require math
require fio
require rtti

let MA_SAMPLE_RATE = 48000
let MA_CHANNELS = 2

class AudioSource
    bitrate : int = MA_SAMPLE_RATE  // samples per second
    channels : int = 1              // 1 or 2
    def ready : bool
        return true
    def abstract get_samples ( nsamples:int ) : array<float>

class AudioChannel
    paused : bool = false
    pitch  : float = 1.
    source : AudioSource?
    resampler : ma_resampler
    channel_converter : ma_channel_converter
    def AudioChannel ( src:AudioSource? )
        source = src
        // resampler
        var resampler_config <- ma_resampler_config_init(
            ma_format ma_format_f32,
            uint(source.channels),
            uint(source.bitrate),
            uint(MA_SAMPLE_RATE),
            ma_resample_algorithm ma_resample_algorithm_linear
        )
        ma_resampler_init(unsafe(addr(resampler_config)), unsafe(addr(resampler)))
        // channel converter
        var channel_converter_config <- ma_channel_converter_config_init(
            ma_format ma_format_f32,
            uint(source.channels),
            null,
            uint(MA_CHANNELS),
            null,
            ma_channel_mix_mode ma_channel_mix_mode_default
        )
        ma_channel_converter_init(unsafe(addr(channel_converter_config)), unsafe(addr(channel_converter)))
    def finalize
        unsafe
            delete source
        ma_channel_converter_uninit(unsafe(addr(channel_converter)))
        ma_resampler_uninit(unsafe(addr(resampler)))
    def mix ( var data:array<float>#; channels,rate:int; dt:float ) : bool
        if paused || !source->ready()
            return true
        let inputRate = uint(float(source.bitrate) * pitch)
        ma_resampler_set_rate(unsafe(addr(resampler)), inputRate, uint(rate))
        var outputSamples = uint64(data|>length/channels)
        var inputSamples = ma_resampler_get_required_input_frame_count(unsafe(addr(resampler)), outputSamples)
        var samples : array<float>
        if inputSamples != outputSamples
            // resample
            samples |> resize(int(outputSamples))
            var temp <- source->get_samples(int(inputSamples))
            if length(temp) < int(inputSamples)
                temp |> resize(int(inputSamples))    // fill up with 0.
            ma_resampler_process_pcm_frames(unsafe(addr(resampler)),
                unsafe(addr(temp[0])),
                unsafe(addr(inputSamples)),
                unsafe(addr(samples[0])),
                unsafe(addr(outputSamples)))
            delete temp
        else
            // fill samples directly
            samples <- source->get_samples(int(inputSamples))
        // convert channels
        ma_channel_converter_process_pcm_frames(unsafe(addr(channel_converter)),
            unsafe(addr(data[0])),
            unsafe(addr(samples[0])),
            outputSamples)
        unsafe
            delete samples
        return true

class AudioChannel2D : AudioChannel
    pan : float = 0.     // -1 left to 1 right

class AudioChannel3D : AudioChannel
    position : float3       // meters
    velocity : float3       // meters per second
    attenuation : float

var g_channels : array<AudioChannel?>
var g_mixer_total_time = 0.lf
var g_mixer_total_samples = 0ul

class AudioSourceMonoGenerator : AudioSource
    gen : iterator<float>
    def AudioSourceMonoGenerator ( var g:iterator<float>; rate:int )
        bitrate = rate
        gen <- g
    def override get_samples ( nsamples:int ) : array<float>
        var f : float
        var i : int
        var data : array<float>
        data |> reserve(nsamples)
        while gen|>next(f) && i<nsamples
            data |> push(f)
            i ++
        return <- data

def remove_channel ( srci:int )
    unsafe
        delete g_channels[srci]
    g_channels |> erase(srci)

def command_processor
    pass

def mixer ( var data:array<float>#; channels,rate:int; dt:float )
    let t0 = ref_time_ticks()
    command_processor()
    let srct = g_channels |> length
    for i in range(srct)
        let srci = srct - i - 1
        if !g_channels[srci]->mix(data,channels,rate,dt)
            remove_channel(srci)
    g_mixer_total_time += double(get_time_usec(t0)) / 1000.lf
    g_mixer_total_samples += uint64(length(data)/channels)

[init]
def initialize_mixer
    if this_context().category.audio
        var monoGen = new AudioSourceMonoGenerator(gen_sine_wave(440.,6000),6000)
        var channel = new AudioChannel(monoGen)
        g_channels |> push(channel)

[finalize]
def finalize_mixer
    if this_context().category.audio
        let SPEED = g_mixer_total_time / double(g_mixer_total_samples)
        let SPEED_OF_LIGHT = 1000.lf / 48000.lf
        let UTILIZATION = int(SPEED / SPEED_OF_LIGHT * 1000.lf)
        to_log(LOG_INFO, "mixer {UTILIZATION/10}.{UTILIZATION%10}% utilization\n")

def gen_sine_wave ( freq:float; rate:int )
    return <- generator<float> () <|
        var t = 0.
        var dt = freq / float(rate)
        while true
            yield sin(t * (2.*PI))
            t = (t + dt) % 1.
        return false

[export]
def main
    sound_initalize(@@mixer,MA_SAMPLE_RATE,MA_CHANNELS,this_context())
    sleep(1000u)
    sound_finalize()
