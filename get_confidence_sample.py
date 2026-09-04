import os
from volcenginesdkarkruntime import Ark
import base64
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('-video_path', type=str)
parser.add_argument('-prompt', type=str)
args = parser.parse_args()

client = Ark(
    base_url='https://ark.cn-beijing.volces.com/api/v3',
    api_key="",
)
# Convert local files to Base64-encoded strings.
def encode_file(file_path):
  with open(file_path, "rb") as read_file:
    return base64.b64encode(read_file.read()).decode('utf-8')
base64_file = encode_file(args.video_path)

response = client.responses.create(
    model="doubao-seed-1-8-251228",
    input=[
        {
            "role": "user",
            "content": [
                {    
                    "type": "input_video",
                    "video_url": f"data:video/mp4;base64,{base64_file}",
                    "fps":2
                },
                {
                    "type": "input_text",
                    "text": args.prompt
                }
            ],
        }
    ]
)
content = response.output[-1].content[0].text
# print(response)
print(content)