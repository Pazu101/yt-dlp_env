# ROADMAP
* Have the new videos from subscriptions listed in the terminal and let used choose the ones to download.
* Cleaner logs folder naming.
* Once the video is downloaded, delete it from the watch later playlist.
* Add the name of the yt chanel at the end of the filename.

# YouTube Watch Later Downloader

A small vibe-coded C program that provides a cleaner and more controlled environment around `yt-dlp` for downloading a YouTube Watch Later playlist.

The goal is mainly **personal and educational use** (I plan to rewrite the program in Python myself and add features later, see the Roadmap below).

## Features

* Clean, curated terminal output
* Separate detailed and human-readable logs
* Automatic log cleanup
* Watch Later archive tracking
* POT server management
* Quality safeguard requiring at least 1080p when the POT server is unavailable
* Download progress and final run summary
* Automatic cleanup of the POT server when finished
* Uses concurrent fragments for faster downloads

The actual video extraction and downloading is still handled by **yt-dlp**.

## Requirements

The following must be installed and available in your `PATH`:

* `yt-dlp`
* `node`
* `curl`

The `bgutil-ytdlp-pot-provider` server is also required.

## Compilation

```bash
clang -O2 -Wall -Wextra -o ytdl_wl ytdl_wl.c
```

Then:

```bash
./ytdl_wl
```

## Configuration

Paths and a few settings are currently defined directly in the C source, including:

* Download directory
* POT server location
* Log location
* Download archive
* Browser used for cookies
* Log retention periods
* Quality settings

These should be adjusted before running the program on another machine.

## About

This is an independent C implementation of a personal yt-dlp-based workflow, created with Claude assistance from the original Python implementation and specific requirements for the desired behavior.

It is **not affiliated with or endorsed by the yt-dlp project**.

## License

Released under the **Unlicense**. See [`LICENSE`](LICENSE).
