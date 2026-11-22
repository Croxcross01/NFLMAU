#!/usr/bin/env python3
"""Madden NFL Arcade PC launcher: pick your own game dump, build once, then play."""
import json, os, pathlib, subprocess, sys, threading
import tkinter as tk
from tkinter import filedialog, scrolledtext, messagebox

ROOT = pathlib.Path(__file__).resolve().parent
BUILD = ROOT / 'build'
SETTINGS = ROOT / 'launcher.json'
EXE = BUILD / 'host' / ('madden_host.exe' if os.name == 'nt' else 'madden_host')

def load_settings():
    try: return json.loads(SETTINGS.read_text())
    except Exception: return {}
def save_settings(s):
    try: SETTINGS.write_text(json.dumps(s, indent=2))
    except Exception: pass

def find_exe():
    if EXE.exists(): return EXE
    for p in (BUILD / 'host').rglob(EXE.name) if (BUILD / 'host').exists() else []: return p
    return None

class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('Madden NFL Arcade PC'); self.geometry('760x520'); self.minsize(560, 380)
        self.s = load_settings(); self.proc = None
        top = tk.Frame(self, padx=12, pady=10); top.pack(fill='x')
        tk.Label(top, text='Your game dump (XBLA package file or extracted folder):').pack(anchor='w')
        row = tk.Frame(top); row.pack(fill='x', pady=4)
        self.dump = tk.StringVar(value=self.s.get('dump', ''))
        tk.Entry(row, textvariable=self.dump).pack(side='left', fill='x', expand=True)
        tk.Button(row, text='File...', command=self.pick_file).pack(side='left', padx=(6, 0))
        tk.Button(row, text='Folder...', command=self.pick_dir).pack(side='left', padx=(6, 0))
        btns = tk.Frame(top); btns.pack(fill='x', pady=(8, 0))
        self.build_btn = tk.Button(btns, text='1. Build from my dump', width=22, command=self.build)
        self.build_btn.pack(side='left')
        self.play_btn = tk.Button(btns, text='2. Play', width=12, command=self.play)
        self.play_btn.pack(side='left', padx=8)
        self.status = tk.Label(btns, text='', fg='#555'); self.status.pack(side='left', padx=8)
        self.log = scrolledtext.ScrolledText(self, height=20, font=('Consolas' if os.name == 'nt' else 'Monospace', 9))
        self.log.pack(fill='both', expand=True, padx=12, pady=(4, 12))
        self.refresh()
        self.write('Use your own legally obtained copy of Madden NFL Arcade (Xbox 360 XBLA).\n'
                   'The build happens on this computer; nothing from the game is downloaded or uploaded.\n')

    def refresh(self):
        built = find_exe() is not None and (BUILD / 'game' / 'Arcadegame.img').exists()
        self.play_btn.config(state='normal' if built else 'disabled')
        self.status.config(text='Ready to play' if built else 'Not built yet')
    def write(self, t):
        self.log.insert('end', t); self.log.see('end')
    def pick_file(self):
        p = filedialog.askopenfilename(title='Select your Madden NFL Arcade XBLA package')
        if p: self.dump.set(p)
    def pick_dir(self):
        p = filedialog.askdirectory(title='Select the folder with your extracted game files')
        if p: self.dump.set(p)

    def stream(self, cmd, done):
        def worker():
            try:
                self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                for line in self.proc.stdout: self.after(0, self.write, line)
                code = self.proc.wait()
            except Exception as e:
                self.after(0, self.write, f'\nerror: {e}\n'); code = -1
            self.after(0, done, code)
        threading.Thread(target=worker, daemon=True).start()

    def build(self):
        d = self.dump.get().strip()
        if not d or not os.path.exists(d): messagebox.showerror('Madden NFL Arcade PC', 'Choose your game dump first.'); return
        self.s['dump'] = d; save_settings(self.s)
        self.build_btn.config(state='disabled'); self.status.config(text='Building... (this takes a while)')
        def done(code):
            self.build_btn.config(state='normal'); self.refresh()
            self.write('\nBuild finished.\n' if code == 0 else f'\nBuild failed (exit {code}). See the log above.\n')
        self.stream([sys.executable, str(ROOT / 'setup.py'), '--dump', d, '--build', str(BUILD)], done)

    def play(self):
        exe = find_exe()
        if not exe: return
        self.status.config(text='Running')
        self.stream([str(exe), '--game', str(BUILD / 'game'), '--save', str(BUILD / 'save')],
                    lambda code: (self.status.config(text=f'Game exited ({code})'), self.refresh()))

if __name__ == '__main__':
    Launcher().mainloop()
