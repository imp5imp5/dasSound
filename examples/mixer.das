options persistent_heap

require sound
require math
require fio
require rtti
require debugapi

require daslib/array_boost
require daslib/jobque_boost
require daslib/apply_in_context

// audio config
let MA_SAMPLE_RATE = 48000
let MA_CHANNELS = 2

// limiter config
let MA_LIMITER_THRESHOLD = 1.0
let MA_LIMITER_ATTACK_TIME = 0.005
let MA_LIMITER_RELEASE_TIME = 0.100

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
    volume_mixer : ma_volume_mixer
    playback_position : uint64 = 0ul
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
        // volume mixer
        ma_volume_mixer_init(unsafe(addr(volume_mixer)),1u)
    def finalize
        unsafe
            delete source
        ma_volume_mixer_uninit(unsafe(addr(volume_mixer)))
        ma_channel_converter_uninit(unsafe(addr(channel_converter)))
        ma_resampler_uninit(unsafe(addr(resampler)))
    def mix ( var data:array<float>#; channels,rate:int; dt:float ) : bool
        if paused || !source->ready()
            return true
        let inputRate = uint(float(source.bitrate) * pitch)
        ma_resampler_set_rate(unsafe(addr(resampler)), inputRate, uint(rate))
        var outputSamples = uint64(data|>length/channels)
        var inputSamples = ma_resampler_get_required_input_frame_count(unsafe(addr(resampler)), outputSamples)
        var samples <- source->get_samples(int(inputSamples))
        if length(samples) < int(inputSamples)
            samples |> resize(int(inputSamples))
        if inputSamples != outputSamples
            // resample
            var temp : array<float>
            temp |> resize(int(outputSamples))
            ma_resampler_process_pcm_frames(unsafe(addr(resampler)),
                unsafe(addr(samples[0])),
                unsafe(addr(inputSamples)),
                unsafe(addr(temp[0])),
                unsafe(addr(outputSamples)))
            delete samples
            samples <- temp
        // convert channels
        var channel_data : array<float>
        channel_data |> resize(data |> length)
        ma_channel_converter_process_pcm_frames(unsafe(addr(channel_converter)),
            unsafe(addr(channel_data[0])),
            unsafe(addr(samples[0])),
            outputSamples)
        unsafe
            delete samples
        // apply volume envelope
        ma_volume_mixer_set_channels(unsafe(addr(volume_mixer)), uint(channels))
        ma_volume_mixer_process_pcm_frames(
            unsafe(addr(volume_mixer)),
            unsafe(addr(channel_data[0])),
            unsafe(addr(data[0])),
            outputSamples)
        delete channel_data
        // update position
        playback_position += inputSamples
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

let MA_RESULT_OK = 0

class AudioSourceDecoder : AudioSource
    decoder : ma_decoder
    isReady : bool = false
    def AudioSourceDecoder
        pass
    def initFromFile ( filename:string; rate,channels:int )
        var config <- ma_decoder_config_init(ma_format ma_format_f32, uint(channels), uint(rate))
        let result = ma_decoder_init_file(filename, unsafe(addr(config)), unsafe(addr(decoder)))
        if result == MA_RESULT_OK
            isReady = true
    def finalize
        if isReady
            ma_decoder_uninit(unsafe(addr(decoder)))
    def override ready : bool
        return isReady
    def override get_samples ( nsamples:int ) : array<float>
        var samples : array<float>
        samples |> resize(nsamples)
        let nframes = ma_decoder_read_pcm_frames(
            unsafe(addr(decoder)),
            unsafe(addr(samples[0])),
            uint64(nsamples))
        return <- samples

def remove_channel ( srci:int )
    unsafe
        delete g_channels[srci]
    g_channels |> erase(srci)

variant AudioCommand
    shutdown    : bool
    add_decoder : tuple<filename:string;rate:int;nChannels:int>

var g_command_channel : Channel?

[pinvoke, export]
def setup_command_processor ( ch:Channel? )
    g_command_channel = ch
    g_command_channel |> add_ref

def command_processor
    return if g_command_channel==null
    let nCmd = g_command_channel.total
    for _ in range(nCmd)
        g_command_channel |> pop_one <| $ ( cmd:AudioCommand# )
            if cmd is shutdown
                g_command_channel |> notify_and_release
            elif cmd is add_decoder
                var dcmd = cmd as add_decoder
                var decoder  = new AudioSourceDecoder()
                decoder->initFromFile(string(dcmd.filename),dcmd.rate,dcmd.nChannels)
                var channel = new AudioChannel(decoder)
                g_channels |> push(channel)

var g_limiter : ma_limiter
var g_mix_buffer : array<float>

[export]
def mixer ( var data:array<float>#; channels,rate:int; dt:float )
    let t0 = ref_time_ticks()
    command_processor()
    let output_samples = length(data)/channels
    let input_samples = int(ma_limiter_get_required_input_frame_count(unsafe(addr(g_limiter)),uint64(output_samples)))
    let current_samples = length(g_mix_buffer)/channels
    let missing_samples = input_samples - current_samples
    if missing_samples < 0
        panic("{input_samples} - {current_samples}, need {output_samples}; mixer: missing_samples < 0")
    g_mix_buffer |> resize(input_samples*channels)
    array_view(g_mix_buffer, current_samples * channels, missing_samples * channels) <| $ ( mix_data )
        let srct = g_channels |> length
        for i in range(srct)
            let srci = srct - i - 1
            if !g_channels[srci]->mix(mix_data,channels,rate,dt)
                remove_channel(srci)
    ma_limiter_process_pcm_frames(unsafe(addr(g_limiter)),
        unsafe(addr(g_mix_buffer[0])),
        unsafe(addr(data[0])),
        uint64(output_samples))
    g_mix_buffer |> erase(0, output_samples * channels)
    g_mixer_total_time += double(get_time_usec(t0)) / 1000.lf
    g_mixer_total_samples += uint64(length(data)/channels)

[init]
def initialize_mixer
    if this_context().category.audio
        this_context().name := "audio_mixer"
        ma_limiter_init(unsafe(addr(g_limiter)),
            MA_LIMITER_THRESHOLD,
            MA_LIMITER_ATTACK_TIME,
            MA_LIMITER_RELEASE_TIME,
            float(MA_SAMPLE_RATE),
            uint(MA_CHANNELS))

[finalize]
def finalize_mixer
    if this_context().category.audio
        ma_limiter_uninit(unsafe(addr(g_limiter)))
        delete g_mix_buffer
        let SPEED = g_mixer_total_time / double(g_mixer_total_samples)
        let SPEED_OF_LIGHT = 1000.lf / 48000.lf
        let UTILIZATION = int(SPEED / SPEED_OF_LIGHT * 1000.lf)
        to_log(LOG_INFO, "mixer {UTILIZATION/10}.{UTILIZATION%10}% utilization\n")

def with_audio_system ( blk:block )
    sound_initalize(@@mixer,MA_SAMPLE_RATE,MA_CHANNELS,this_context())
    with_channel(1) <| $ ( channel )
        g_command_channel = channel
        unsafe
            invoke_in_context(mixer_context(),"setup_command_processor",channel)
        invoke(blk)
        channel |> push_clone <| [[AudioCommand shutdown=true]]
        g_command_channel = null
        channel |> join
    sound_finalize()

def gen_sine_wave ( freq:float; rate:int )
    return <- generator<float> () <|
        var t = 0.
        var dt = freq / float(rate)
        while true
            yield sin(t * (2.*PI))
            t = (t + dt) % 1.
        return false

def play_sound_from_file ( filename:string; rate,channels:int )
    g_command_channel |> push_clone <| [[AudioCommand add_decoder=[[auto filename, rate, channels]]]]

[export]
def main
    with_audio_system <|
        sleep(100u)
        play_sound_from_file("{get_das_root()}/modules/dasSound/examples/test.mp3", MA_SAMPLE_RATE, 2)
        sleep(3000u)




