from flask import Flask, request, render_template, url_for, send_file
from flask import after_this_request
import subprocess, os, uuid
from datetime import datetime

app = Flask(__name__)
OUTPUT_DIR = 'outputs'
os.makedirs(OUTPUT_DIR, exist_ok=True)

@app.route('/', methods=['GET'])
def index():
    return render_template('index.html', current_year=datetime.now().year)

@app.route('/upload', methods=['POST'])
def upload():
    video = request.files.get('video')
    if not video:
        return "No video uploaded", 400

    # 1) Save input + produce output paths
    uid      = uuid.uuid4().hex
    in_path  = os.path.join(OUTPUT_DIR, f"{uid}.mp4")
    ppm_path = os.path.join(OUTPUT_DIR, f"{uid}.ppm")
    png_path = os.path.join(OUTPUT_DIR, f"{uid}.png")

    video.save(in_path)

    # 2) Run chromashot_fast → PPM, then ffmpeg → PNG
    subprocess.run(['./chromashot_fast', in_path, ppm_path], check=True, timeout=300)
    subprocess.run(['ffmpeg', '-y', '-i', ppm_path, png_path],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 3) Clean up the MP4 + PPM immediately
    os.remove(in_path)
    os.remove(ppm_path)

    # 4) Render the page, pointing to the inline image route
    image_url = url_for('serve_image', uid=uid)
    return render_template('index.html',
                           current_year=datetime.now().year,
                           image_url=image_url)

@app.route('/image/<uid>.png')
def serve_image(uid):
    png_path = os.path.join(OUTPUT_DIR, f"{uid}.png")
    if not os.path.exists(png_path):
        return "Not found", 404

    # Stream the file inline, then delete it when done
    @after_this_request
    def cleanup(response):
        try:
            os.remove(png_path)
        except OSError:
            pass
        return response

    return send_file(png_path, mimetype='image/png')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8000)
