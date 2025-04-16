# 1) base image
FROM python:3.10-slim

# 2) install OS deps and FFmpeg dev libs
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      build-essential pkg-config ffmpeg \
      libavformat-dev libavcodec-dev libavfilter-dev \
      libavutil-dev libswscale-dev && \
    rm -rf /var/lib/apt/lists/*

# 3) copy everything
WORKDIR /app
COPY . /app

# 4) compile your C tool
RUN gcc -std=c99 -O3 -march=native chromashot_fast.c -o chromashot_fast \
      $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale) -lm

# 5) install Flask
RUN pip install --no-cache-dir flask

# 6) expose & start
EXPOSE 8000
CMD ["gunicorn", "-b", "0.0.0.0:8000", "app:app"]
