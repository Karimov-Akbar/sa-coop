#!/usr/bin/env python3
"""
Checks that the game client shows no Russian text without its English one (python tools/i18n_check.py, exit code 1
when one is found).

A Russian string literal in src/client must stand inside TR("...", "...") or TR_BOTH("...", "...")
(shared/config/Config.h), inside a log call (LOGI, LOGW, LOGE, LOGD: the log is for us) or in the comments of the
default coop.ini. The server needs no check of
this kind: it sends its texts in both languages and the compiler sees each pair together.
"""
import re
import sys
from pathlib import Path

CLIENT = Path(__file__).resolve().parent.parent / "src" / "client"
ALLOWED = {"TR", "TR_BOTH", "LOGI", "LOGW", "LOGE", "LOGD"}
RUSSIAN = re.compile("[А-Яа-яЁё]")
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|L?"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|[(){}]|[A-Za-z_]\w*', re.S)


def check(text):
    """(line, literal) of every Russian literal of text outside the allowed calls."""
    errors, stack, last_word = [], [], None
    ini_depth, ini_next = 0, False  # inside the body of WriteDefaultConfig: its literals are coop.ini's comments
    for m in TOKEN.finditer(text):
        tok = m.group()
        if tok.startswith("//") or tok.startswith("/*"):
            continue
        if tok == "{" and (ini_next or ini_depth):
            ini_depth, ini_next = ini_depth + 1, False
        elif tok == "}" and ini_depth:
            ini_depth -= 1
        elif tok == "(":
            stack.append(last_word)
        elif tok == ")":
            if stack:
                stack.pop()
        elif tok.startswith('"') or tok.startswith('L"'):
            if RUSSIAN.search(tok) and not ini_depth and not ALLOWED & set(stack):
                errors.append((text.count("\n", 0, m.start()) + 1, tok))
        if tok == "WriteDefaultConfig" and last_word == "void":
            ini_next = True
        last_word = tok if (tok[0].isalpha() or tok[0] == "_") else None
    return errors


def self_test():
    ok = 'a(TR("Да",\n "Yes")); LOGI("Готово %d", 1); Format(TR("%s ушёл", "%s left"), n);'
    assert check(ok) == [], check(ok)
    bad = 'chat.Add("Привет"); x = TR("a", "b") + "Ещё";'
    assert [line for line, _ in check(bad)] == [1, 1], check(bad)
    ini = 'void WriteDefaultConfig() { if (a) { t = "; Ник"; } t += "Да"; } f("Нет"); WriteDefaultConfig(x); g("Ой");'
    assert [lit for _, lit in check(ini)] == ['"Нет"', '"Ой"'], check(ini)


if __name__ == "__main__":
    self_test()
    found = 0
    for f in sorted(CLIENT.rglob("*.cpp")):
        for line, lit in check(f.read_text(encoding="utf-8")):
            print(f"{f.relative_to(CLIENT).as_posix()}:{line}: {lit} has no English text (TR)")
            found += 1
    sys.exit(1 if found else 0)
