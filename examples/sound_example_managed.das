require sound
require medialib/dasbox_sound_utils
require math
require fio

[export]
def main
    sound_initialize()
    var signal <- [{ for i in range(30000); sin(float(i) * 0.1) * 0.2 }]
    var snd <- create_managed_sound(48000, signal)
    var handle = play_sound(snd)
    while is_playing(handle)
        sleep(50u)
    sound_finalize()
