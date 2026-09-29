#!/usr/bin/env python3
"""screen: the V in ir-kvm. Grab what the capture stick sees.

  screen grab [-o out.jpg] [--mode 1080p10|720p30]   one frame to a file (default /tmp/irkvm-screen.jpg)
  screen serve [--mode ...]                            keep the latest frame in /dev/shm/irkvm-screen.jpg
  screen stop                                          stop the serve process
  screen info                                          which /dev/video is the stick, modes, service state

grab uses the served frame when the service is running (milliseconds), otherwise opens the
device itself (about 2 s including sensor warm-up). Modes are what a MacroSilicon MS2109 on a
USB 2 port can actually deliver: 1080p10 (best for reading text) or 720p30 (motion). 1080p30 and
720p60 overrun the bus and decode as garbage.
"""
import argparse, glob, os, shutil, subprocess, sys, time

SHM = '/dev/shm/irkvm-screen.jpg'
PIDFILE = '/dev/shm/irkvm-screen.pid'
MODES = {'1080p10': ('1920x1080', '10'), '720p30': ('1280x720', '30')}
CAPTURE_NAMES = ('USB Video', 'MACROSILICON', 'MS2109', 'MS2130', 'HDMI')

def find_device():
    if os.environ.get('IRKVM_VIDEO'): return os.environ['IRKVM_VIDEO']
    for v in sorted(glob.glob('/dev/video*'), key=lambda p: int(p[10:])):
        try: name = open(f'/sys/class/video4linux/{os.path.basename(v)}/name').read().strip()
        except OSError: continue
        if any(n.lower() in name.lower() for n in CAPTURE_NAMES):
            # UVC exposes a capture node and a metadata node; the capture node has formats
            r = subprocess.run(['v4l2-ctl', '-d', v, '--list-formats'], capture_output=True, text=True)
            if 'MJPG' in r.stdout or 'YUYV' in r.stdout: return v
    sys.exit('no HDMI capture device found (set IRKVM_VIDEO=/dev/videoN)')

def ffmpeg_base(dev, mode):
    size, fps = MODES[mode]
    return ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'v4l2', '-input_format', 'mjpeg',
            '-video_size', size, '-framerate', fps, '-i', dev]

def served_fresh(max_age):
    try: return time.time() - os.stat(SHM).st_mtime < max_age
    except OSError: return False

def grab(out, mode):
    if served_fresh(2.0):
        shutil.copyfile(SHM, out); return 'served'
    dev = find_device()
    warmup = 2 * int(MODES[mode][1])   # the stick emits blank frames for ~1.5 s after streaming starts
    cmd = ffmpeg_base(dev, mode) + ['-vf', f'select=gte(n\\,{warmup})', '-frames:v', '1', '-update', '1', '-y', out]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if r.returncode or not os.path.exists(out): sys.exit(f'grab failed: {r.stderr.strip()[-300:]}')
    return 'direct'

def serve(mode):
    dev = find_device()
    tmp = SHM + '.tmp'
    # MJPEG passthrough (-c:v copy): no decode, negligible CPU; write to a temp name and rename
    # so readers never see a partial file. ffmpeg's image2 muxer handles this with -atomic_writing.
    # -c:v copy cannot drop frames, so the file updates at the mode's native rate (10 or 30 fps); harmless on /dev/shm
    cmd = ffmpeg_base(dev, mode) + ['-fps_mode', 'passthrough', '-c:v', 'copy', '-f', 'image2', '-update', '1',
                                    '-atomic_writing', '1', '-y', SHM]
    stop()
    print(f'serving {dev} {mode} -> {SHM}', flush=True)
    open(PIDFILE, 'w').write(str(os.getpid()))
    os.execvp('ffmpeg', cmd)

def stop():
    try: pid = int(open(PIDFILE).read())
    except (OSError, ValueError): return False
    try:
        if 'irkvm-screen' in open(f'/proc/{pid}/cmdline').read(): os.kill(pid, 15); time.sleep(0.3)
    except OSError: pass
    try: os.unlink(PIDFILE)
    except OSError: pass
    return True

def info():
    dev = find_device()
    print('device:', dev)
    print('service:', 'running (frame %.1fs old)' % (time.time() - os.stat(SHM).st_mtime) if served_fresh(5) else 'not running')
    subprocess.run(['v4l2-ctl', '-d', dev, '--get-fmt-video'])

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('grab'); g.add_argument('-o', '--out', default='/tmp/irkvm-screen.jpg'); g.add_argument('--mode', choices=MODES, default='1080p10')
    s = sub.add_parser('serve'); s.add_argument('--mode', choices=MODES, default='1080p10')
    sub.add_parser('stop'); sub.add_parser('info')
    a = ap.parse_args()
    if a.cmd == 'grab':
        t0 = time.time(); how = grab(a.out, a.mode)
        print(f'{a.out} ({how}, {time.time()-t0:.2f}s)')
    elif a.cmd == 'serve': serve(a.mode)
    elif a.cmd == 'stop': print('stopped' if stop() else 'not running')
    else: info()

if __name__ == '__main__':
    main()
