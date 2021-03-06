#!/bin/sh

usage() {
  printf "Usage: %s video_size pixel_format framerate base_output_filename\n" "$0"
}

# validate num params
if ! (( $# == 4 )); then
  usage
  exit -1
fi

echo "cmdline: $@" >> /tmp/pipe_ffmpeg.out

#
# tweaking suggesting:
# Depending on your hardware, you might be able to use an higher quality
# preset. Possible value for preset are:
#
# - superfast
# - veryfast
# - faster
# - fast
#
schedtool -I -e ffmpeg -nostats -f rawvideo -video_size $1 -pixel_format $2 -framerate $3 -i /dev/stdin \
 -f alsa -acodec pcm_s16le -ar 48000 -ac 2 -i loop_capture \
 -c:a libfdk_aac -profile:a aac_low -b:a 128k -ar 44100 \
 -c:v libx264 -preset superfast -profile:v main -level 4.1 -pix_fmt yuv420p \
 -x264opts keyint=60:bframes=2:ref=1 -maxrate 4500k -bufsize 9000k -shortest $4.mkv \
 &>> /tmp/pipe_ffmpeg.out 

