"""Run native question endpoint and actual production upload-loop failure tests."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'artifacts/wake-runtime'


def main():
    source = (ROOT / 'BendeRadio/BenderAi.cpp').read_text(encoding='utf-8')
    start = source.index('static bool flushRecording() {')
    end = source.index('static void pttStartCapture()', start)
    (OUT / 'flush_recording_under_test.h').write_text(source[start:end], encoding='utf-8')
    start = source.index('static BenderPlayback::State playbackState() {')
    end = source.index('void bender_ai_begin()', start)
    (OUT / 'playback_tick_under_test.h').write_text(source[start:end], encoding='utf-8')
    start = source.index('static void playWakeReadyCue() {')
    end = source.index('static bool readMicChunk16(', start)
    (OUT / 'wake_reply_under_test.h').write_text(source[start:end], encoding='utf-8')
    nvs = (ROOT / 'BendeRadio/NvsConfig.cpp').read_text(encoding='utf-8')
    start = nvs.index('bool nvsLoadWakeVoiceEnabled() {')
    end = nvs.index('uint8_t nvsLoadWakeFollowupSeconds()', start)
    (OUT / 'wake_preference_under_test.h').write_text(nvs[start:end], encoding='utf-8')
    start = source.index('static void prePush(const uint8_t* pcm) {')
    end = source.index('static void preTrimIdle()', start)
    initial = source[start:end]
    start = source.index('static void scaleWakeChunk(uint8_t* bytes) {')
    end = source.index('static void wsTask(void*)', start)
    (OUT / 'wake_initial_under_test.h').write_text(initial+source[start:end], encoding='utf-8')
    env = dict(os.environ, ZIG_LOCAL_CACHE_DIR=str(OUT / 'zig-cache'),
               ZIG_GLOBAL_CACHE_DIR=str(OUT / 'zig-global-cache'))
    for name in ('wake_voice_test', 'wake_upload_test', 'playback_completion_test', 'playback_tick_test', 'wake_reply_test', 'wake_preference_test', 'wake_initial_capture_test'):
        exe = OUT / (name + '.exe')
        subprocess.run([str(OUT / 'tools/ziglang/zig.exe'), 'c++', '-std=c++17', '-O2',
                        str(ROOT / 'tests' / (name + '.cpp')), '-o', str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
