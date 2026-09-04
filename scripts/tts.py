from pocket_tts import TTSModel
import scipy.io.wavfile

# directory that resulting .wav files are stored in
wav_dir = "path/to/directory"

model = TTSModel.load_model()
voice_state = model.get_state_for_audio_prompt("charles")

def makeWav(text: str):
    """
    Function that accepts some text and runs it through text-to-speech, exporting the result to a .wav file
    Args:
        text: string text to convert to speech
    Returns:
        str: path to resulting .wav file
    Raises:
        ValueError: if text is None
    """
    if not text:
        raise ValueError("Text is empty")

    audio = model.generate_audio(voice_state, text)
    fname = f"{wav_dir}/{text.replace(" ", "_")}.wav"
    scipy.io.wavfile.write(fname, model.sample_rate, audio.numpy())

    return fname

