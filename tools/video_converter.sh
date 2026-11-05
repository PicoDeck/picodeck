#!/bin/bash

CROP_VIDEO=false
VIDEO_QUALITY=8
AUDIO_MUTED=false
# 4:2:0 chroma halves the decoder's chroma work on the RP2350 (ffmpeg only
# picks it by itself when the source is already 4:2:0); --chroma444 keeps
# full-resolution colour at roughly 2x decode time.
PIX_FMT=yuvj420p

# Check if ffmpeg is installed
if ! command -v ffmpeg &> /dev/null; then
    echo "ffmpeg is not installed. Please install it and try again."
    exit 1
fi

usage() {
    echo "Usage: $0 [--crop] [--quality N] [--mute] [--chroma444] <video_file_or_url>"
    exit 1
}

while [[ "$1" == -* ]]; do
    case "$1" in
        --crop)
            CROP_VIDEO=true
            shift
            ;;
        --quality)
            shift
            VIDEO_QUALITY="$1"
            shift
            ;;
        --mute)
            AUDIO_MUTED=true
            shift
            ;;
        --chroma444)
            PIX_FMT=yuvj444p
            shift
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Only after the options: the input may follow any of them.
FILE="$1"
if [ -z "$FILE" ]; then
    usage
fi

if [ "$AUDIO_MUTED" = true ]; then
    AUDIO_OPTS="-an"
else
    AUDIO_OPTS="-acodec libmp3lame -ab 128k -ar 22050"
fi


# If input starts with http, use yt-dlp to download
if [[ "$FILE" =~ ^http ]]; then
    # Check if yt-dlp is installed for downloading videos from URLs
    if ! command -v yt-dlp &> /dev/null; then
        echo "yt-dlp is not installed. Please install it to download videos from URLs."
        exit 1
    fi

    # A directory, not an empty temp file: yt-dlp takes a file that already
    # exists as finished and downloads nothing.
    DOWNLOAD_DIR=$(mktemp -d)
    trap 'rm -rf "$DOWNLOAD_DIR"' EXIT
    echo "Downloading $FILE with yt-dlp..."
    yt-dlp -f best --restrict-filenames -o "$DOWNLOAD_DIR/%(title)s.%(ext)s" "$FILE"
    if [ $? -ne 0 ]; then
        echo "yt-dlp failed to download video."
        exit 1
    fi
    FILE=$(find "$DOWNLOAD_DIR" -type f | head -n 1)
    if [ -z "$FILE" ]; then
        echo "yt-dlp wrote no file."
        exit 1
    fi
fi

OUTPUT_DIR="$HOME/Videos/PicoDeck/"
if [ ! -d "$OUTPUT_DIR" ]; then
    echo "Output directory $OUTPUT_DIR does not exist."
    exit 1
fi

FILENAME=$(basename "$FILE")
EXTENSION="${FILENAME##*.}"
BASENAME="${FILENAME%.*}"

echo "Converting $FILE to AVI format for PicoDeck video player..."




if [ "$CROP_VIDEO" = true ]; then
    ffmpeg -i "$FILE" \
        -vf "fps=25,scale=320:320:force_original_aspect_ratio=increase,crop=320:320" \
        -vcodec mjpeg \
        -pix_fmt $PIX_FMT \
        -q:v $VIDEO_QUALITY \
        -huffman optimal \
        $AUDIO_OPTS \
        "$OUTPUT_DIR/${BASENAME}.avi"
else
    ffmpeg -i "$FILE" \
        -vf "fps=25,scale=320:-1" \
        -vcodec mjpeg \
        -pix_fmt $PIX_FMT \
        -q:v $VIDEO_QUALITY \
        -huffman optimal \
        $AUDIO_OPTS \
        "$OUTPUT_DIR/${BASENAME}.avi"
fi
