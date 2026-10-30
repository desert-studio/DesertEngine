#!/usr/bin/env python3
"""UserPromptSubmit: when the owner asks how something should be built, remind the lead to answer as an architect.

Owner 10-30: «ты часто просто выкидываешь идею, которая может быть не лучшая по архитектуре … я всё время на неё
опираюсь». The brief check in agent_guard.py («## Архитектура») covers what reaches an agent; this covers the chat
answer, which no tool call guards. It fires only on design questions, so it costs nothing on routine messages.
"""
import json
import re
import sys

DESIGN = re.compile(
    r"архитектур|как (лучше |правильно )?(сделать|устроить|реализовать|хранить)|где (должн|жить)|"
    r"должн[аоыя]? (ли |быть )|движ(ок|ке) или|скрипт(ах|ы)? или|ассет(ом)?\?|хардкод|как (это )?делает ue|"
    r"какой (подход|вариант)|почему так|лучше ли",
    re.I)

REMINDER = (
    "[arch_prompt] Вопрос про устройство. Владелец опирается на ответ как на решение (10-30), поэтому ответ должен "
    "быть решением, а не первой идеей: 1) Эталон: как это сделано у UE/AAA (проверено, а не по памяти); "
    "2) Варианты: что ещё возможно и почему отброшено; 3) Выбор и в чём он лучше UE. Если эталон не проверен — "
    "назвать ответ «набросок» и запустить разбор, не выдавать идею за решение.")


def main():
    try:
        prompt = json.load(sys.stdin).get("prompt", "")
    except (ValueError, OSError):
        return
    if DESIGN.search(prompt):
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": REMINDER}},
                         ensure_ascii=False))


if __name__ == "__main__":
    main()
