"""Convert generated speech WAV to compact embedded 8 kHz PCM."""
import audioop
from pathlib import Path
import wave
root = Path(__file__).resolve().parents[1]
text = '#pragma once\n#include <stdint.h>\n'
for name in ('start', 'stop'):
    with wave.open(str(root / f'build/recording_{name}.wav'), 'rb') as wav:
        assert wav.getnchannels() == 1 and wav.getsampwidth() == 2
        pcm, _ = audioop.ratecv(wav.readframes(wav.getnframes()), 2, 1, wav.getframerate(), 8000, None)
    # Signed 8 bit PCM, expanded to 16 bit by the audio task.
    pcm = audioop.lin2lin(pcm, 2, 1)
    text += f'static const int8_t s_voice_{name}[] = {{\n'
    for offset in range(0, len(pcm), 32):
        text += ','.join(str(v if v < 128 else v - 256) for v in pcm[offset:offset+32]) + ',\n'
    text += '};\n'
    print(name, len(pcm), 'samples at 8 kHz')
(root / 'main/recording_voice_pcm.h').write_text(text, encoding='utf-8')
