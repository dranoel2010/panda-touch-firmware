#!/usr/bin/env python3
"""Zugangsdaten für den Drucker abfragen und in main/secrets.h schreiben (nie im Git).

Aufruf: python3 tools/zugangsdaten.py
Enter ohne Eingabe behält den bisherigen Wert.
"""
import getpass
import os
import re

PFAD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'main', 'secrets.h')
FELDER = [
    ('WLAN_SSID', 'WLAN-Name', False),
    ('WLAN_PASSWORT', 'WLAN-Passwort', True),
    ('DRUCKER_IP', 'Drucker-IP (P1S: Einstellungen → WLAN)', False),
    ('DRUCKER_ACCESS_CODE', 'Access Code (P1S: Einstellungen → WLAN)', True),
    ('DRUCKER_SERIENNUMMER', 'Seriennummer (P1S: Einstellungen → Gerät)', False),
]

alt = {}
if os.path.exists(PFAD):
    for m in re.finditer(r'#define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"', open(PFAD, encoding='utf-8').read()):
        alt[m.group(1)] = m.group(2).encode().decode('unicode_escape').encode('latin-1').decode('utf-8')

neu = {}
for name, frage, geheim in FELDER:
    bisher = alt.get(name, '')
    hinweis = (' [gesetzt, Enter = behalten]' if geheim else f' [{bisher}]') if bisher else ''
    eingabe = (getpass.getpass if geheim else input)(f'{frage}{hinweis}: ').strip()
    neu[name] = eingabe or bisher


def c_text(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


with open(PFAD, 'w', encoding='utf-8') as f:
    f.write('/* Zugangsdaten (diese Datei ist im .gitignore und kommt nie ins Git). */\n#pragma once\n')
    for name, _, _ in FELDER:
        f.write(f'#define {name:<21} {c_text(neu[name])}\n')
print('Gespeichert in main/secrets.h' + ('' if all(neu.values()) else ' – Achtung: nicht alle Felder ausgefüllt'))
