# Love Show

A Windows desktop "music video" experience that plays a song, reveals synced lyrics, and turns the entire screen into a romantic animated performance.

Built in C++ with Win32 APIs, this project creates a floating popup window that displays lyrical lines as the song plays, while the desktop responds with motion, warm color tinting, heart effects, and playful cursor-driven visual chaos.

## Highlights

- Plays an audio file and follows the song timeline automatically
- Loads standard LRC lyrics from `lyrics.lrc`
- Reveals lyric text in a typewriter-style cadence
- Animates a floating popup window with a dramatic romantic theme
- Applies screen tinting, heartbeat glow, particle hearts, and shake effects
- Minimizes other open windows and restores them during the performance
- Adds interactive motion such as a cursor-following window and a dodging "OK" button
- Lets you stop the experience at any time by pressing `Esc`

## Requirements

- Windows 10 or later
- A C++ compiler for Windows
- `song.mp3` placed next to the executable
- `lyrics.lrc` placed next to the executable

## Project files

- `main.cpp` — the full Win32 application
- `song.mp3` — the song to play
- `lyrics.lrc` — synced lyrics in standard LRC format
- `lyrics.txt` — optional plain-text lyric reference

## Build instructions

### MSVC

```bat
cl /EHsc /O2 /utf-8 main.cpp
```

### MinGW / GCC

```bat
g++ -O2 -mwindows main.cpp -o love_show.exe -lwinmm -lgdi32 -luser32 -ldwmapi
```

## Usage

1. Place `song.mp3` and `lyrics.lrc` next to the generated `.exe`.
2. Run `love_show.exe`.
3. The app will load the audio track and lyrics automatically.
4. Press `Esc` at any time to stop the experience.

## LRC format

The lyrics should follow the standard LRC structure, for example:

```text
[00:12.34]I think they call this love
[00:16.78]and everything goes still
[00:24.12]when the world starts glowing
```

Important notes:

- Use UTF-8 encoding for the lyrics file.
- Metadata tags like `[ti:...]`, `[ar:...]`, and similar entries are ignored.
- Lines without timestamps are skipped.

## Notes

This project is designed as a stylized single-purpose desktop visualizer rather than a general-purpose media player. It is intentionally crafted around a specific song and its timing so the lyric reveal and visual effects line up with the music.

If you want to reuse it for another song, update the `song.mp3` file and replace `lyrics.lrc` with a new synced lyric track.

Note: The song currently in the repo is not mine. The song and lyrics belong to Matthew Ifield.


## GitHub repo description

A Windows lyric-synced desktop music video built in C++ with Win32, featuring animated lyrics, motion effects, and a dramatic full-screen romantic experience.
