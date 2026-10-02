# Sets the intensity-stereo bit (and keeps the M/S bit) in every joint-stereo
# Layer III frame header of an MP3: argv[1] -> argv[2].
import sys
BR1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
BR2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
SR = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}
b = bytearray(open(sys.argv[1], 'rb').read())
i = n = k = 0
while i + 4 <= len(b):
    if b[i] == 0xFF and (b[i + 1] & 0xE0) == 0xE0:
        ver = (b[i + 1] >> 3) & 3; layer = (b[i + 1] >> 1) & 3
        bri = b[i + 2] >> 4; sri = (b[i + 2] >> 2) & 3; pad = (b[i + 2] >> 1) & 1
        if layer == 1 and ver != 1 and 0 < bri < 15 and sri < 3:
            br = (BR1 if ver == 3 else BR2)[bri] * 1000; sr = SR[ver][sri]
            ln = (144 if ver == 3 else 72) * br // sr + pad
            if b[i + 3] >> 6 == 1:
                b[i + 3] |= 0x10
                n += 1
            k += 1
            i += ln
            continue
    i += 1
open(sys.argv[2], 'wb').write(b)
print(sys.argv[2], 'intensity stereo in', n, 'of', k, 'frames')
