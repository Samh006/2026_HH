from openrouter import OpenRouter
import base64
import mimetypes
import sys
import os

def transcribe(img_path):
    # encode the image in a base64 url
    with open(img_path, "rb") as img_file:
        base64_string = base64.b64encode(img_file.read()).decode("utf-8")

    #base64_url = f"data:image/jpeg;base64,{base64_string}"
    base64_url = f"data:{mimetypes.guess_type(img_path)};base64,{base64_string}"

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
                                "url": base64_url
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
        text = transcribe(sys.argv[1])
        sys.stdout.write(text)
        sys.stdout.flush()