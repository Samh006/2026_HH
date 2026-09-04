from openrouter import OpenRouter
import base64
import mimetypes
import sys
import os

def makeBase64Url(file_path: str):
    """
    Function that accepts path to a file, encodes it in a base64 and returns the resulting base64 url
    Args:
        file_path: string path of the file to be encoded
    Returns:
        str: string base64 url
    Raises:
        ValueError: if the MIME type of the provided file could not be determined
    """
    file_mime, _ = mimetypes.guess_type(file_path)

    # raises an exception if MIME type could not be guessed
    if not file_mime:
        raise ValueError(f"Could not determine MIME type of {file_path}")

    with open(file_path, "rb") as file:
        b64_str = base64.b64encode(file.read()).decode("utf-8")

    return f"data:{file_mime};base64,{b64_str}"

def transcribe(url):
    with OpenRouter(
        api_key = os.getenv("OPENROUTER_API_KEY", "") # openrouter API key stored in environment variable
    ) as client:
        response = client.chat.send(
            model = "google/gemma-4-31b-it",
            messages = [
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "text",
                            "text": "Transcribe the text in this image"
                        },
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": url
                            }
                        }
                    ]
                }
            ],
            provider = {
                "only": ["cerebras"]
            }
        )

    return response.choices[0].message.content

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("ERROR: No path provided, usage: python transcribe.py <path/to/image>")
    else:
        url = makeBase64Url(sys.argv[1])
        text = transcribe(url)
        sys.stdout.write(text)
        sys.stdout.flush()