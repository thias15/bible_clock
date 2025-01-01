#!/usr/bin/env python3

"""
This script can parse Bible JSON from two different sources:

1) https://www.biblesupersearch.com/bible-downloads/
   - Format: { "metadata": {...}, "verses": [ {...}, {...} ] }

2) https://github.com/jadenzaleski/BibleTranslations
   - Format: { "Genesis": { "1": {"1": "...", "2": "..."}, "2": {...} }, "Exodus": {...}, ... }

For each hour/minute of the day (24 × 60 = 1440):
- If no verses exist at that time => Generate a short encouraging GPT statement.
- If 1 or more verses exist => Ask GPT to pick the best verse. 
  - If GPT says "none" and there's exactly 1 verse, we generate a GPT statement.
  - If GPT says "none" and there are multiple verses, we pick a random verse 
    (to still use a real verse and "prioritize bible verses if possible").
- For minute=0 (full hour), always a GPT statement with reference "HH:00" (no parentheses).

We preserve:
- Parentheses ( ) in references when bible verse is chosen.
- Apostrophes '.
- The verse text starts uppercase and ends with '.' or '...'.
- TQDM progress bars for hours/minutes.
No "GPTGenerated" reference is ever used.
"""

import json
import random
import os
import re
from openai import OpenAI
from tqdm import tqdm  # pip install tqdm

openai_api_key = os.getenv("OPENAI_API_KEY", "")
if not openai_api_key:
    print("Warning: No OPENAI_API_KEY found. Please set env var or hardcode for testing.")
client = OpenAI(api_key=openai_api_key)

# -----------------------------------------------------------------------------
# 1) parse_jadenzaleski_bible (GitHub/jadenzaleski/BibleTranslations)
# -----------------------------------------------------------------------------
def parse_jadenzaleski_bible(json_data):
    """
    Convert a nested Bible JSON of the form:
      {
        "Genesis": {
          "1": { "1": "text...", "2": "text..." },
          "2": { "1": "text..." }
        },
        "Exodus": { ... }
      }
    into a list of dicts for each verse:
      [
        {"book_name": "Genesis", "chapter": 1, "verse": 1, "text": "..."},
        ...
      ]
    """
    verses_list = []
    for book_name, chapters_dict in json_data.items():
        for chapter_str, verses_dict in chapters_dict.items():
            chapter_num = int(chapter_str)
            for verse_str, verse_text in verses_dict.items():
                verse_num = int(verse_str)
                verses_list.append({
                    "book_name": book_name,
                    "chapter": chapter_num,
                    "verse": verse_num,
                    "text": verse_text
                })
    return verses_list

# -----------------------------------------------------------------------------
# 2) GPT Utility
# -----------------------------------------------------------------------------

recent_statements = []  # Store up to 5 of the last accepted statements

def ask_gpt_for_encouraging_statement() -> str:
    """
    Calls GPT (gpt-4o-mini or similar) for a short, uplifting Christian statement.
    We embed the last 5 accepted statements in the system prompt, telling GPT to
    avoid reusing them or sounding too similar. We also remind GPT to:
      - remain consistent with biblical truth,
      - employ synonyms, figurative speech, etc.
    """

    # Build a short "recent statements" string to show GPT
    # If we have fewer than 5, we'll show them all. If we have more, we slice the last 5.
    last_5 = recent_statements[-5:]
    joined_recent = "\n".join(f"- {st}" for st in last_5)

    # Construct the system prompt
    system_prompt = (
        "You are a helpful AI that writes short, uplifting, encouraging Christian statements. "
        "They must be concise, easy to read, and in plain language. "
        "Always remain consistent with biblical truth, but employ synonyms, figurative speech, "
        "and varied sentence structures. Avoid repeating the same ideas in the same way.\n\n"

        "Here are up to five recent statements we already used:\n"
        f"{joined_recent}\n\n"

        "Please produce ONE new short, encouraging Christian statement or verse. "
        "If it conveys a similar idea to the recent ones, please rephrase and vary the wording."
    )

    user_prompt = "Write a short, encouraging Christian statement or verse that doesn't repeat the recent statements."

    try:
        response = client.chat.completions.create(
            model="gpt-4o",   # or "gpt-3.5-turbo", "gpt-4", etc.
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
            temperature=0.9,       # Encourage more variety
            top_p=1.0
        )
        candidate_text = response.choices[0].message.content.strip()

    except Exception as e:
        print(f"[ERROR] GPT request failed: {e}")
        # fallback if API call fails
        candidate_text = "Trust in the Lord with all your heart, and let His love guide your steps."

    # Update the recent statements buffer
    recent_statements.append(candidate_text)
    if len(recent_statements) > 5:
        recent_statements.pop(0)

    return candidate_text

def pick_best_verse(candidate_verses):
    """
    If we have 1..N candidate verses, ask GPT to pick the best.
      - If GPT says 'none':
         * If exactly 1 verse => generate a GPT statement
         * If multiple verses => pick a random verse
    """
    if not candidate_verses:
        return None

    # Build system prompt
    system_prompt = (
        "You are a helpful AI that picks the best short, uplifting Christian verse among the candidates.\n"
        "We WANT a verse that is short, meaningful, easy to read and understand.\n"
        "If there are multiple options, pick the most encouraging one.\n"
        "We do NOT want verses which require further context to make sense or just list names, places, etc.\n\n"
        "Examples of acceptable verses:\n"
        "- For God so loved the world that he gave his only son.\n"
        "- And he said to them, Why were you looking for me?\n"
        "- No, I tell you but unless you repent, you will all likewise perish.\n\n"
        "Examples of not acceptable:\n"
        "- The sons of Neziah, and the sons of Hatipha.\n"
        "- The following were those who came up from Telmelah...\n"
        "- Jezreel, Jokdeam, Zanoah...\n\n"
        "We will provide a list of candidate verses; respond ONLY with the index (1-based), or 'none' if none are acceptable."
    )

    user_content = "Here are the candidate verses:\n\n"
    for i, v in enumerate(candidate_verses, start=1):
        user_content += f"{i}) {v['text']}\n"
    user_content += "\nWhich one is the best? Return only the number or 'none'."

    try:
        response = client.chat.completions.create(
            model="gpt-4o",
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_content},
            ],
            temperature=0.0
        )
        raw_reply = response.choices[0].message.content.strip().lower()

        if raw_reply.startswith("none"):
            # If exactly 1 verse => generate GPT statement
            if len(candidate_verses) == 1:
                return None  # means we'll do GPT statement
            else:
                # If multiple verses => pick random among them
                return random.choice(candidate_verses)

        # parse an integer index
        try:
            chosen_index = int(raw_reply) - 1
        except ValueError:
            chosen_index = 0

        if chosen_index < 0 or chosen_index >= len(candidate_verses):
            # If out of range => pick random
            return random.choice(candidate_verses)

        return candidate_verses[chosen_index]

    except Exception as e:
        print(f"[ERROR] GPT picking verse failed: {e}")
        # fallback => pick random
        return random.choice(candidate_verses)

# -----------------------------------------------------------------------------
# 3) Sanitization + Format Fix
# -----------------------------------------------------------------------------

def sanitize_string(s):
    """
    Replaces German letters, keeps parentheses, apostrophes, 
    and removes other special characters.
    """
    # Keep apostrophes ('), parentheses ( ), punctuation .,:?! plus letters/digits/spaces
    s = s.replace("Ä", "Ae").replace("Ö", "Oe").replace("Ü", "Ue")
    s = s.replace("ä", "ae").replace("ö", "oe").replace("ü", "ue").replace("ß", "ss")
    s = re.sub(r'[^a-zA-Z0-9 \.:,\?\!\(\)\']', '', s)
    return s

def fix_verse_format(text: str) -> str:
    """
    Ensures:
      1) Capital letter at start (if not empty).
      2) Ends with '.' or if it ends with ',' => '...'.
    """
    t = text.strip()
    if not t:
        return t

    # Capitalize first char
    t = t[0].upper() + t[1:] if len(t) > 1 else t.upper()

    # Check last char
    last_char = t[-1]
    if last_char == ',':
        t = t[:-1].rstrip() + '...'
    elif last_char not in {'.', '!', '?'}:
        t += '.'

    return t

# -----------------------------------------------------------------------------
# 4) Main
# -----------------------------------------------------------------------------
def main():
    """
    Usage:
      python script.py biblesupersearch path/to/bible.json
      python script.py jadenzaleski path/to/another_bible.json

    Where:
      - 'biblesupersearch' expects { "metadata":{...}, "verses":[...]}
        from https://www.biblesupersearch.com/bible-downloads/
      - 'jadenzaleski' expects { "Genesis": {...}, "Exodus": {...}}
        from https://github.com/jadenzaleski/BibleTranslations
    """
    import sys

    if len(sys.argv) < 3:
        print("Usage: python script.py <format> <json_file>")
        print("  format = 'biblesupersearch' or 'jadenzaleski'")
        sys.exit(1)

    bible_format = sys.argv[1]   # 'biblesupersearch' or 'jadenzaleski'
    bible_json_file = sys.argv[2]

    # 1) Load JSON
    with open(bible_json_file, "r", encoding="utf-8") as f:
        data = json.load(f)

    # 2) Parse according to chosen format
    if bible_format == "biblesupersearch":
        all_verses = data["verses"]  # e.g. {"metadata":{...}, "verses":[...]}
    elif bible_format == "jadenzaleski":
        all_verses = parse_jadenzaleski_bible(data)
    else:
        print(f"Unknown format: {bible_format}")
        sys.exit(1)

    # 3) Build (chapter, verse) -> list of verse dicts
    chap_verse_map = {}
    for v in all_verses:
        c = v["chapter"]
        r = v["verse"]
        key = (c, r)
        if key not in chap_verse_map:
            chap_verse_map[key] = []
        chap_verse_map[key].append(v)

    # 4) Create output folder
    out_dir = "data"
    if not os.path.exists(out_dir):
        os.makedirs(out_dir)

    # 5) Loop over 24 hours × 60 minutes with progress bars
    for hour in tqdm(range(1, 25), desc="Processing Hours"):
        hour_result = {}

        for minute in tqdm(range(60), desc=f"Hour {hour}", leave=False):
            minute_str = f"{minute:02d}"

            # Full hour => always GPT statement
            if minute == 0:
                reference_str = f"{hour:02d}:{minute:02d}"
                from_gpt = ask_gpt_for_encouraging_statement()
                final_txt = fix_verse_format(sanitize_string(from_gpt))

                hour_result[minute_str] = {
                    "reference": sanitize_string(reference_str),
                    "text": final_txt
                }
                continue

            cv_key = (hour, minute)
            # If no verses => GPT statement
            if cv_key not in chap_verse_map:
                raw_ref = f"{hour:02d}:{minute:02d}"
                from_gpt = ask_gpt_for_encouraging_statement()
                final_txt = fix_verse_format(sanitize_string(from_gpt))

                hour_result[minute_str] = {
                    "reference": sanitize_string(raw_ref),
                    "text": final_txt
                }
            else:
                # We do have verses => ask GPT to pick best
                possible_verses = chap_verse_map[cv_key]
                if len(possible_verses) > 20:
                    candidate_verses = random.sample(possible_verses, 20)
                else:
                    candidate_verses = possible_verses

                chosen = pick_best_verse(candidate_verses)
                if chosen is None:
                    # Means GPT said "none" but there's exactly 1 verse
                    # => generate a GPT statement
                    raw_ref = f"{hour:02d}:{minute:02d}"
                    from_gpt = ask_gpt_for_encouraging_statement()
                    final_txt = fix_verse_format(sanitize_string(from_gpt))

                    hour_result[minute_str] = {
                        "reference": sanitize_string(raw_ref),
                        "text": final_txt
                    }
                else:
                    # GPT picked or we fallback to random among multiples
                    raw_ref = f"{hour:02d}:{minute:02d} ({chosen['book_name']})"
                    ref_ok = sanitize_string(raw_ref)
                    text_ok = sanitize_string(chosen["text"])
                    text_ok = fix_verse_format(text_ok)

                    hour_result[minute_str] = {
                        "reference": ref_ok,
                        "text": text_ok
                    }

        # Save JSON for this hour
        filename = os.path.join(out_dir, f"bible_verses_hour{hour:02d}.json")
        with open(filename, "w", encoding="utf-8") as out_f:
            json.dump(hour_result, out_f, ensure_ascii=False, indent=2)

    print("\nDone! Created 24 separate JSON files in 'data' folder.")

if __name__ == "__main__":
    main()
